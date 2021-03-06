#include "Teuchos_Reader.hpp"

#include <iostream>
#include <sstream>
#include <fstream>
#include <ios>
#include <cstdlib>
#include <set>

#include "Teuchos_string.hpp"
#include "Teuchos_vector.hpp"
#include "Teuchos_Parser.hpp"

namespace Teuchos {

namespace {

void print_indicator(std::ostream& os, std::string const& above, std::size_t pos) {
  for (std::size_t i = 0; i < pos; ++i) {
    if (above.at(i) == '\t') os << '\t';
    else os << ' ';
  }
  os << "^\n";
}

void print_underline(std::ostream& os, std::string const& above, std::size_t start, std::size_t end) {
  for (std::size_t i = 0; i < start; ++i) {
    if (above.at(i) == '\t') os << '\t';
    else os << ' ';
  }
  for (std::size_t i = start; i < end; ++i) os << '~';
  os << '\n';
}

} // end anonymous namespace

Reader::IndentStackEntry::IndentStackEntry(std::size_t l, std::size_t s, std::size_t e):
  line(l),start_length(s),end_length(e) {
}

void Reader::at_token() {
  bool done = false;
  /* this can loop arbitrarily as reductions are made,
     because they don't consume the token */
  while (!done) {
    const Action& parser_action = get_action(parser, parser_state, lexer_token);
    if (parser_action.kind == ACTION_NONE) {
      std::stringstream ss;
      ss << "error: Parser failure at line " << line;
      ss << " column " << column << " of " << stream_name << '\n';
      ss << line_text << '\n';
      print_indicator(ss, line_text, line_text.size());
      std::set<std::string> expect_names;
      for (int expect_token = 0;
           expect_token < grammar->nterminals; ++expect_token) {
        const Action& expect_action = get_action(parser, parser_state, expect_token);
        if (expect_action.kind != ACTION_NONE) {
          expect_names.insert(at(grammar->symbol_names, expect_token));
        }
      }
      ss << "Expected one of {";
      for (std::set<std::string>::iterator it = expect_names.begin();
           it != expect_names.end(); ++it) {
        if (it != expect_names.begin()) ss << ", ";
        if (*it == ",") ss << "','";
        else ss << *it;
      }
      ss << "}\n";
      ss << "Got: " << at(grammar->symbol_names, lexer_token) << '\n';
      ss << "Parser was in state " << parser_state << '\n';
      throw ParserFail(ss.str());
    } else if (parser_action.kind == ACTION_SHIFT) {
      Teuchos::any shift_result;
      this->at_shift(shift_result, lexer_token, lexer_text);
      add_back(value_stack, shift_result);
      done = true;
    } else if (parser_action.kind == ACTION_REDUCE) {
      if (parser_action.production == get_accept_production(*grammar)) {
        did_accept = true;
        return;
      }
      const Grammar::Production& prod = at(grammar->productions, parser_action.production);
      reduction_rhs.clear();
      for (int i = 0; i < size(prod.rhs); ++i) {
        add_back(reduction_rhs, at(value_stack, size(value_stack) - size(prod.rhs) + i));
      }
      resize(value_stack, size(value_stack) - size(prod.rhs));
      Teuchos::any reduce_result;
      this->at_reduce(reduce_result, parser_action.production, reduction_rhs);
      add_back(value_stack, reduce_result);
    } else {
      TEUCHOS_TEST_FOR_EXCEPTION(true, std::logic_error,
          "SERIOUS BUG: Action::kind enum value not in range\n");
    }
    parser_state = execute_action(parser, parser_stack, parser_action);
  }
}

void Reader::indent_mismatch() {
  TEUCHOS_ASSERT(!indent_stack.empty());
  const IndentStackEntry& top = indent_stack.back();
  std::stringstream ss;
  ss << "error: Indentation characters beginning line " << line << " of " << stream_name
    << " don't match those beginning line " << top.line << '\n';
  ss << "It is strongly recommended not to mix tabs and spaces in indentation-sensitive formats\n";
  throw ParserFail(ss.str());
}

void Reader::at_token_indent() {
  if (!sensing_indent || lexer_token != tables->indent_info.nodent_token) {
    at_token();
    return;
  }
  TEUCHOS_ASSERT(at(lexer_text, 0) == '\n');
  std::string lexer_indent = lexer_text.substr(1, std::string::npos);
  std::size_t minlen = std::min(lexer_indent.length(), indent_text.length());
  if (lexer_indent.length() > indent_text.length()) {
    if (0 != lexer_indent.compare(0, indent_text.length(), indent_text)) {
      indent_mismatch();
    }
    indent_stack.push_back(IndentStackEntry(line, indent_text.length(), lexer_indent.length()));
    indent_text = lexer_indent;
    lexer_token = tables->indent_info.indent_token;
    at_token();
  } else if (lexer_indent.length() < indent_text.length()) {
    if (0 != indent_text.compare(0, lexer_indent.length(), lexer_indent)) {
      indent_mismatch();
    }
    bool first = true;
    while (!indent_stack.empty()) {
      const IndentStackEntry& top = indent_stack.back();
      if (top.end_length <= minlen) break;
      indent_stack.pop_back();
      lexer_token = tables->indent_info.dedent_token;
      at_token();
      if (first) {
        lexer_text.clear();
        first = false;
      }
    }
    if (first) lexer_text.clear();
    indent_text = lexer_indent;
  } else {
    if (0 != lexer_indent.compare(indent_text)) {
      indent_mismatch();
    }
    lexer_token = tables->indent_info.eqdent_token;
    at_token();
  }
}

void Reader::backtrack_to_last_accept(std::istream& stream) {
  /* all the last_accept and backtracking is driven by
    the "accept the longest match" rule */
  line = last_lexer_accept_line;
  column = last_lexer_accept_column;
  line_text = last_lexer_accept_line_text;
  while (lexer_text.size() > last_lexer_accept) {
    bool ok = !stream.unget().fail();
    TEUCHOS_ASSERT(ok);
    resize(lexer_text, size(lexer_text) - 1);
  }
}

void Reader::reset_lexer_state() {
  lexer_state = 0;
  lexer_text.clear();
  lexer_token = -1;
}

void Reader::at_lexer_end(std::istream& stream) {
  if (lexer_token == -1) {
    std::stringstream ss;
    if (lexer_text.find('\n') == std::string::npos) {
      ss << "error: Could not tokenize this (line " <<  line;
      ss << " column " << column << " of " << stream_name << "):\n";
      ss << line_text << '\n';
      TEUCHOS_ASSERT(line_text.size() >= lexer_text.size());
      print_underline(ss, line_text, line_text.size() - lexer_text.size(), line_text.size());
    } else {
      ss << "error: Could not tokenize this (ends at line " << line;
      ss << " column " << column << " of " << stream_name << "):\n";
      ss << lexer_text << '\n';
    }
    throw ParserFail(ss.str());
  }
  backtrack_to_last_accept(stream);
  at_token_indent();
  reset_lexer_state();
}

Reader::Reader(ReaderTablesPtr tables_in):
  tables(tables_in),
  parser(tables->parser),
  lexer(tables->lexer),
  grammar(get_grammar(parser))
{
  TEUCHOS_ASSERT(get_determinism(lexer));
}

void Reader::update_position(char c) {
  if (c == '\n') {
    ++line;
    column = 1;
    line_text.clear();
  } else {
    ++column;
  }
}

void Reader::read_stream(any& result, std::istream& stream, std::string const& stream_name_in) {
  using std::swap;
  line = 1;
  column = 1;
  lexer_state = 0;
  lexer_text.clear();
  line_text.clear();
  lexer_token = -1;
  parser_state = 0;
  parser_stack.clear();
  parser_stack.push_back(parser_state);
  value_stack.clear();
  did_accept = false;
  stream_name = stream_name_in;
  if (tables->indent_info.is_sensitive) {
    sensing_indent = true;
    indent_text.clear();
    indent_stack.clear();
    /* pretend the stream starts with a newline so we can
       detect an INDENT on the first line. don't update the
       line/column pointers though. */
    char c = '\n';
    lexer_text.push_back(c);
    int lexer_symbol = get_symbol(c);
    lexer_state = step(lexer, lexer_state, lexer_symbol);
    TEUCHOS_ASSERT(lexer_state != -1);
    lexer_token = accepts(lexer, lexer_state);
    TEUCHOS_ASSERT(lexer_token == tables->indent_info.nodent_token);
    last_lexer_accept = lexer_text.size();
    last_lexer_accept_line = 1;
    last_lexer_accept_column = 1;
    last_lexer_accept_line_text = line_text;
  } else {
    sensing_indent = false;
  }
  char c;
  while (stream.get(c)) {
    if (!is_symbol(c)) {
      std::stringstream ss;
      ss << "error: Unexpected character code " << int(c);
      ss << " at line " << line << " column " << column;
      ss << " of " << stream_name << '\n';
      if (!line_text.empty()) {
        ss << line_text << '\n';
        print_indicator(ss, line_text, line_text.size());
      }
      throw ParserFail(ss.str());
    }
    line_text.push_back(c);
    lexer_text.push_back(c);
    int lexer_symbol = get_symbol(c);
    lexer_state = step(lexer, lexer_state, lexer_symbol);
    if (lexer_state == -1) {
      at_lexer_end(stream);
    } else {
      int token = accepts(lexer, lexer_state);
      update_position(c);
      if (token != -1) {
        lexer_token = token;
        last_lexer_accept = lexer_text.size();
        last_lexer_accept_line = line;
        last_lexer_accept_column = column;
        last_lexer_accept_line_text = line_text;
      }
    }
  }
  if (last_lexer_accept < lexer_text.size()) {
    std::stringstream ss;
    std::string bad_str = lexer_text.substr(last_lexer_accept, std::string::npos);
    ss << "error: Could not tokenize \"" << bad_str;
    ss << "\" at end of " << stream_name << '\n';
    throw ParserFail(ss.str());
  }
  at_lexer_end(stream);
  lexer_token = get_end_terminal(*grammar);
  at_token();
  TEUCHOS_TEST_FOR_EXCEPTION(!did_accept, std::logic_error,
      "The EOF terminal was accepted but the root nonterminal was not reduced\n"
      "This indicates a bug in Teuchos::Reader\n");
  TEUCHOS_ASSERT(value_stack.size() == 1);
  swap(result, value_stack.back());
}

void Reader::read_string(any& result, std::string const& string, std::string const& string_name) {
  std::istringstream stream(string);
  read_stream(result, stream, string_name);
}

void Reader::read_file(any& result, std::string const& file_name) {
  std::ifstream stream(file_name.c_str());
  read_stream(result, stream, file_name);
}

void Reader::at_shift(any&, int, std::string&) {
}

void Reader::at_reduce(any&, int, std::vector<any>&) {
}

DebugReader::DebugReader(ReaderTablesPtr tables_in, std::ostream& os_in):
  Reader(tables_in),os(os_in)
{
}

void DebugReader::at_shift(any& result, int token, std::string& text) {
  std::string& text_escaped = make_any_ref<std::string>(result);
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    switch (c) {
      case '\n': text_escaped.append("\\n"); break;
      case '\t': text_escaped.append("\\t"); break;
      case '\r': text_escaped.append("\\r"); break;
      default: text_escaped.push_back(c);
    }
  }
  os << "SHIFT (" << at(grammar->symbol_names, token) << ")[" << text_escaped << "]\n";
}

void DebugReader::at_reduce(any& result, int prod_i, std::vector<any>& rhs) {
  os << "REDUCE";
  std::string& lhs_text = make_any_ref<std::string>(result);
  const Grammar::Production& prod = at(grammar->productions, prod_i);
  for (int i = 0; i < size(prod.rhs); ++i) {
    const std::string& rhs_name = at(grammar->symbol_names, at(prod.rhs, i));
    const std::string& rhs_text = any_ref_cast<std::string>(at(rhs, i));
    os << " (" << rhs_name << ")[" << rhs_text << "]";
    lhs_text.append(rhs_text);
  }
  const std::string& lhs_name = at(grammar->symbol_names, prod.lhs);
  os << " -> (" << lhs_name << ")[" << lhs_text << "]\n";
}

}  // namespace Teuchos
