// Copyright 2011 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "api.h"
#include "ast.h"
#include "bootstrapper.h"
#include "char-predicates-inl.h"
#include "codegen.h"
#include "compiler.h"
#include "func-name-inferrer.h"
#include "messages.h"
#include "parser.h"
#include "platform.h"
#include "preparser.h"
#include "runtime.h"
#include "scanner-character-streams.h"
#include "scopeinfo.h"
#include "string-stream.h"

namespace v8 {
namespace internal {

// PositionStack is used for on-stack allocation of token positions for
// new expressions. Please look at ParseNewExpression.

class PositionStack  {
 public:
  explicit PositionStack(bool* ok) : top_(NULL), ok_(ok) {}
  ~PositionStack() { ASSERT(!*ok_ || is_empty()); }

  class Element  {
   public:
    Element(PositionStack* stack, int value) {
      previous_ = stack->top();
      value_ = value;
      stack->set_top(this);
    }

   private:
    Element* previous() { return previous_; }
    int value() { return value_; }
    friend class PositionStack;
    Element* previous_;
    int value_;
  };

  bool is_empty() { return top_ == NULL; }
  int pop() {
    ASSERT(!is_empty());
    int result = top_->value();
    top_ = top_->previous();
    return result;
  }

 private:
  Element* top() { return top_; }
  void set_top(Element* value) { top_ = value; }
  Element* top_;
  bool* ok_;
};


RegExpBuilder::RegExpBuilder()
    : zone_(Isolate::Current()->zone()),
      pending_empty_(false),
      characters_(NULL),
      terms_(),
      alternatives_()
#ifdef DEBUG
    , last_added_(ADD_NONE)
#endif
  {}


void RegExpBuilder::FlushCharacters() {
  pending_empty_ = false;
  if (characters_ != NULL) {
    RegExpTree* atom = new(zone()) RegExpAtom(characters_->ToConstVector());
    characters_ = NULL;
    text_.Add(atom);
    LAST(ADD_ATOM);
  }
}


void RegExpBuilder::FlushText() {
  FlushCharacters();
  int num_text = text_.length();
  if (num_text == 0) {
    return;
  } else if (num_text == 1) {
    terms_.Add(text_.last());
  } else {
    RegExpText* text = new(zone()) RegExpText();
    for (int i = 0; i < num_text; i++)
      text_.Get(i)->AppendToText(text);
    terms_.Add(text);
  }
  text_.Clear();
}


void RegExpBuilder::AddCharacter(uc16 c) {
  pending_empty_ = false;
  if (characters_ == NULL) {
    characters_ = new(zone()) ZoneList<uc16>(4);
  }
  characters_->Add(c);
  LAST(ADD_CHAR);
}


void RegExpBuilder::AddEmpty() {
  pending_empty_ = true;
}


void RegExpBuilder::AddAtom(RegExpTree* term) {
  if (term->IsEmpty()) {
    AddEmpty();
    return;
  }
  if (term->IsTextElement()) {
    FlushCharacters();
    text_.Add(term);
  } else {
    FlushText();
    terms_.Add(term);
  }
  LAST(ADD_ATOM);
}


void RegExpBuilder::AddAssertion(RegExpTree* assert) {
  FlushText();
  terms_.Add(assert);
  LAST(ADD_ASSERT);
}


void RegExpBuilder::NewAlternative() {
  FlushTerms();
}


void RegExpBuilder::FlushTerms() {
  FlushText();
  int num_terms = terms_.length();
  RegExpTree* alternative;
  if (num_terms == 0) {
    alternative = RegExpEmpty::GetInstance();
  } else if (num_terms == 1) {
    alternative = terms_.last();
  } else {
    alternative = new(zone()) RegExpAlternative(terms_.GetList());
  }
  alternatives_.Add(alternative);
  terms_.Clear();
  LAST(ADD_NONE);
}


RegExpTree* RegExpBuilder::ToRegExp() {
  FlushTerms();
  int num_alternatives = alternatives_.length();
  if (num_alternatives == 0) {
    return RegExpEmpty::GetInstance();
  }
  if (num_alternatives == 1) {
    return alternatives_.last();
  }
  return new(zone()) RegExpDisjunction(alternatives_.GetList());
}


void RegExpBuilder::AddQuantifierToAtom(int min,
                                        int max,
                                        RegExpQuantifier::Type type) {
  if (pending_empty_) {
    pending_empty_ = false;
    return;
  }
  RegExpTree* atom;
  if (characters_ != NULL) {
    ASSERT(last_added_ == ADD_CHAR);
    // Last atom was character.
    Vector<const uc16> char_vector = characters_->ToConstVector();
    int num_chars = char_vector.length();
    if (num_chars > 1) {
      Vector<const uc16> prefix = char_vector.SubVector(0, num_chars - 1);
      text_.Add(new(zone()) RegExpAtom(prefix));
      char_vector = char_vector.SubVector(num_chars - 1, num_chars);
    }
    characters_ = NULL;
    atom = new(zone()) RegExpAtom(char_vector);
    FlushText();
  } else if (text_.length() > 0) {
    ASSERT(last_added_ == ADD_ATOM);
    atom = text_.RemoveLast();
    FlushText();
  } else if (terms_.length() > 0) {
    ASSERT(last_added_ == ADD_ATOM);
    atom = terms_.RemoveLast();
    if (atom->max_match() == 0) {
      // Guaranteed to only match an empty string.
      LAST(ADD_TERM);
      if (min == 0) {
        return;
      }
      terms_.Add(atom);
      return;
    }
  } else {
    // Only call immediately after adding an atom or character!
    UNREACHABLE();
    return;
  }
  terms_.Add(new(zone()) RegExpQuantifier(min, max, type, atom));
  LAST(ADD_TERM);
}


Handle<String> Parser::LookupSymbol(int symbol_id) {
  // Length of symbol cache is the number of identified symbols.
  // If we are larger than that, or negative, it's not a cached symbol.
  // This might also happen if there is no preparser symbol data, even
  // if there is some preparser data.
  if (static_cast<unsigned>(symbol_id)
      >= static_cast<unsigned>(symbol_cache_.length())) {
    if (scanner().is_literal_ascii()) {
      return isolate()->factory()->LookupAsciiSymbol(
          scanner().literal_ascii_string());
    } else {
      return isolate()->factory()->LookupTwoByteSymbol(
          scanner().literal_uc16_string());
    }
  }
  return LookupCachedSymbol(symbol_id);
}


Handle<String> Parser::LookupCachedSymbol(int symbol_id) {
  // Make sure the cache is large enough to hold the symbol identifier.
  if (symbol_cache_.length() <= symbol_id) {
    // Increase length to index + 1.
    symbol_cache_.AddBlock(Handle<String>::null(),
                           symbol_id + 1 - symbol_cache_.length());
  }
  Handle<String> result = symbol_cache_.at(symbol_id);
  if (result.is_null()) {
    if (scanner().is_literal_ascii()) {
      result = isolate()->factory()->LookupAsciiSymbol(
          scanner().literal_ascii_string());
    } else {
      result = isolate()->factory()->LookupTwoByteSymbol(
          scanner().literal_uc16_string());
    }
    symbol_cache_.at(symbol_id) = result;
    return result;
  }
  isolate()->counters()->total_preparse_symbols_skipped()->Increment();
  return result;
}


FunctionEntry ScriptDataImpl::GetFunctionEntry(int start) {
  // The current pre-data entry must be a FunctionEntry with the given
  // start position.
  if ((function_index_ + FunctionEntry::kSize <= store_.length())
      && (static_cast<int>(store_[function_index_]) == start)) {
    int index = function_index_;
    function_index_ += FunctionEntry::kSize;
    return FunctionEntry(store_.SubVector(index,
                                          index + FunctionEntry::kSize));
  }
  return FunctionEntry();
}


int ScriptDataImpl::GetSymbolIdentifier() {
  return ReadNumber(&symbol_data_);
}


bool ScriptDataImpl::SanityCheck() {
  // Check that the header data is valid and doesn't specify
  // point to positions outside the store.
  if (store_.length() < PreparseDataConstants::kHeaderSize) return false;
  if (magic() != PreparseDataConstants::kMagicNumber) return false;
  if (version() != PreparseDataConstants::kCurrentVersion) return false;
  if (has_error()) {
    // Extra sane sanity check for error message encoding.
    if (store_.length() <= PreparseDataConstants::kHeaderSize
                         + PreparseDataConstants::kMessageTextPos) {
      return false;
    }
    if (Read(PreparseDataConstants::kMessageStartPos) >
        Read(PreparseDataConstants::kMessageEndPos)) {
      return false;
    }
    unsigned arg_count = Read(PreparseDataConstants::kMessageArgCountPos);
    int pos = PreparseDataConstants::kMessageTextPos;
    for (unsigned int i = 0; i <= arg_count; i++) {
      if (store_.length() <= PreparseDataConstants::kHeaderSize + pos) {
        return false;
      }
      int length = static_cast<int>(Read(pos));
      if (length < 0) return false;
      pos += 1 + length;
    }
    if (store_.length() < PreparseDataConstants::kHeaderSize + pos) {
      return false;
    }
    return true;
  }
  // Check that the space allocated for function entries is sane.
  int functions_size =
      static_cast<int>(store_[PreparseDataConstants::kFunctionsSizeOffset]);
  if (functions_size < 0) return false;
  if (functions_size % FunctionEntry::kSize != 0) return false;
  // Check that the count of symbols is non-negative.
  int symbol_count =
      static_cast<int>(store_[PreparseDataConstants::kSymbolCountOffset]);
  if (symbol_count < 0) return false;
  // Check that the total size has room for header and function entries.
  int minimum_size =
      PreparseDataConstants::kHeaderSize + functions_size;
  if (store_.length() < minimum_size) return false;
  return true;
}



const char* ScriptDataImpl::ReadString(unsigned* start, int* chars) {
  int length = start[0];
  char* result = NewArray<char>(length + 1);
  for (int i = 0; i < length; i++) {
    result[i] = start[i + 1];
  }
  result[length] = '\0';
  if (chars != NULL) *chars = length;
  return result;
}

Scanner::Location ScriptDataImpl::MessageLocation() {
  int beg_pos = Read(PreparseDataConstants::kMessageStartPos);
  int end_pos = Read(PreparseDataConstants::kMessageEndPos);
  return Scanner::Location(beg_pos, end_pos);
}


const char* ScriptDataImpl::BuildMessage() {
  unsigned* start = ReadAddress(PreparseDataConstants::kMessageTextPos);
  return ReadString(start, NULL);
}


Vector<const char*> ScriptDataImpl::BuildArgs() {
  int arg_count = Read(PreparseDataConstants::kMessageArgCountPos);
  const char** array = NewArray<const char*>(arg_count);
  // Position after text found by skipping past length field and
  // length field content words.
  int pos = PreparseDataConstants::kMessageTextPos + 1
      + Read(PreparseDataConstants::kMessageTextPos);
  for (int i = 0; i < arg_count; i++) {
    int count = 0;
    array[i] = ReadString(ReadAddress(pos), &count);
    pos += count + 1;
  }
  return Vector<const char*>(array, arg_count);
}


unsigned ScriptDataImpl::Read(int position) {
  return store_[PreparseDataConstants::kHeaderSize + position];
}


unsigned* ScriptDataImpl::ReadAddress(int position) {
  return &store_[PreparseDataConstants::kHeaderSize + position];
}


Scope* Parser::NewScope(Scope* parent, ScopeType type) {
  Scope* result = new(zone()) Scope(parent, type);
  result->Initialize();
  return result;
}


// ----------------------------------------------------------------------------
// Target is a support class to facilitate manipulation of the
// Parser's target_stack_ (the stack of potential 'break' and
// 'continue' statement targets). Upon construction, a new target is
// added; it is removed upon destruction.

class Target BASE_EMBEDDED {
 public:
  Target(Target** variable, AstNode* node)
      : variable_(variable), node_(node), previous_(*variable) {
    *variable = this;
  }

  ~Target() {
    *variable_ = previous_;
  }

  Target* previous() { return previous_; }
  AstNode* node() { return node_; }

 private:
  Target** variable_;
  AstNode* node_;
  Target* previous_;
};


class TargetScope BASE_EMBEDDED {
 public:
  explicit TargetScope(Target** variable)
      : variable_(variable), previous_(*variable) {
    *variable = NULL;
  }

  ~TargetScope() {
    *variable_ = previous_;
  }

 private:
  Target** variable_;
  Target* previous_;
};


// ----------------------------------------------------------------------------
// FunctionState and BlockState together implement the parser's scope stack.
// The parser's current scope is in top_scope_.  The BlockState and
// FunctionState constructors push on the scope stack and the destructors
// pop.  They are also used to hold the parser's per-function and per-block
// state.

class Parser::BlockState BASE_EMBEDDED {
 public:
  BlockState(Parser* parser, Scope* scope)
      : parser_(parser),
        outer_scope_(parser->top_scope_) {
    parser->top_scope_ = scope;
  }

  ~BlockState() { parser_->top_scope_ = outer_scope_; }

 private:
  Parser* parser_;
  Scope* outer_scope_;
};


class Parser::FunctionState BASE_EMBEDDED {
 public:
  FunctionState(Parser* parser, Scope* scope, Isolate* isolate);
  ~FunctionState();

  int NextMaterializedLiteralIndex() {
    return next_materialized_literal_index_++;
  }
  int materialized_literal_count() {
    return next_materialized_literal_index_ - JSFunction::kLiteralsPrefixSize;
  }

  int NextHandlerIndex() { return next_handler_index_++; }
  int handler_count() { return next_handler_index_; }

  void SetThisPropertyAssignmentInfo(
      bool only_simple_this_property_assignments,
      Handle<FixedArray> this_property_assignments) {
    only_simple_this_property_assignments_ =
        only_simple_this_property_assignments;
    this_property_assignments_ = this_property_assignments;
  }
  bool only_simple_this_property_assignments() {
    return only_simple_this_property_assignments_;
  }
  Handle<FixedArray> this_property_assignments() {
    return this_property_assignments_;
  }

  void AddProperty() { expected_property_count_++; }
  int expected_property_count() { return expected_property_count_; }

 private:
  // Used to assign an index to each literal that needs materialization in
  // the function.  Includes regexp literals, and boilerplate for object and
  // array literals.
  int next_materialized_literal_index_;

  // Used to assign a per-function index to try and catch handlers.
  int next_handler_index_;

  // Properties count estimation.
  int expected_property_count_;

  // Keeps track of assignments to properties of this. Used for
  // optimizing constructors.
  bool only_simple_this_property_assignments_;
  Handle<FixedArray> this_property_assignments_;

  Parser* parser_;
  FunctionState* outer_function_state_;
  Scope* outer_scope_;
  unsigned saved_ast_node_id_;
};


Parser::FunctionState::FunctionState(Parser* parser,
                                     Scope* scope,
                                     Isolate* isolate)
    : next_materialized_literal_index_(JSFunction::kLiteralsPrefixSize),
      next_handler_index_(0),
      expected_property_count_(0),
      only_simple_this_property_assignments_(false),
      this_property_assignments_(isolate->factory()->empty_fixed_array()),
      parser_(parser),
      outer_function_state_(parser->current_function_state_),
      outer_scope_(parser->top_scope_),
      saved_ast_node_id_(isolate->ast_node_id()) {
  parser->top_scope_ = scope;
  parser->current_function_state_ = this;
  isolate->set_ast_node_id(AstNode::kDeclarationsId + 1);
}


Parser::FunctionState::~FunctionState() {
  parser_->top_scope_ = outer_scope_;
  parser_->current_function_state_ = outer_function_state_;
  parser_->isolate()->set_ast_node_id(saved_ast_node_id_);
}


// ----------------------------------------------------------------------------
// The CHECK_OK macro is a convenient macro to enforce error
// handling for functions that may fail (by returning !*ok).
//
// CAUTION: This macro appends extra statements after a call,
// thus it must never be used where only a single statement
// is correct (e.g. an if statement branch w/o braces)!

#define CHECK_OK  ok);   \
  if (!*ok) return NULL; \
  ((void)0
#define DUMMY )  // to make indentation work
#undef DUMMY

#define CHECK_FAILED  /**/);   \
  if (failed_) return NULL; \
  ((void)0
#define DUMMY )  // to make indentation work
#undef DUMMY

// ----------------------------------------------------------------------------
// Implementation of Parser

Parser::Parser(Handle<Script> script,
               int parser_flags,
               v8::Extension* extension,
               ScriptDataImpl* pre_data)
    : isolate_(script->GetIsolate()),
      symbol_cache_(pre_data ? pre_data->symbol_count() : 0),
      script_(script),
      scanner_(isolate_->unicode_cache()),
      reusable_preparser_(NULL),
      top_scope_(NULL),
      current_function_state_(NULL),
      target_stack_(NULL),
      extension_(extension),
      pre_data_(pre_data),
      fni_(NULL),
      allow_natives_syntax_((parser_flags & kAllowNativesSyntax) != 0),
      allow_lazy_((parser_flags & kAllowLazy) != 0),
      stack_overflow_(false),
      parenthesized_function_(false) {
  AstNode::ResetIds();
  if ((parser_flags & kLanguageModeMask) == EXTENDED_MODE) {
    scanner().SetHarmonyScoping(true);
  }
}


FunctionLiteral* Parser::ParseProgram(CompilationInfo* info) {
  ZoneScope zone_scope(isolate(), DONT_DELETE_ON_EXIT);

  HistogramTimerScope timer(isolate()->counters()->parse());
  Handle<String> source(String::cast(script_->source()));
  isolate()->counters()->total_parse_size()->Increment(source->length());
  fni_ = new(zone()) FuncNameInferrer(isolate());

  // Initialize parser state.
  source->TryFlatten();
  if (source->IsExternalTwoByteString()) {
    // Notice that the stream is destroyed at the end of the branch block.
    // The last line of the blocks can't be moved outside, even though they're
    // identical calls.
    ExternalTwoByteStringUC16CharacterStream stream(
        Handle<ExternalTwoByteString>::cast(source), 0, source->length());
    scanner_.Initialize(&stream);
    return DoParseProgram(info, source, &zone_scope);
  } else {
    GenericStringUC16CharacterStream stream(source, 0, source->length());
    scanner_.Initialize(&stream);
    return DoParseProgram(info, source, &zone_scope);
  }
}


FunctionLiteral* Parser::DoParseProgram(CompilationInfo* info,
                                        Handle<String> source,
                                        ZoneScope* zone_scope) {
  ASSERT(top_scope_ == NULL);
  ASSERT(target_stack_ == NULL);
  if (pre_data_ != NULL) pre_data_->Initialize();

  // Compute the parsing mode.
  mode_ = (FLAG_lazy && allow_lazy_) ? PARSE_LAZILY : PARSE_EAGERLY;
  if (allow_natives_syntax_ || extension_ != NULL) mode_ = PARSE_EAGERLY;

  Handle<String> no_name = isolate()->factory()->empty_symbol();

  FunctionLiteral* result = NULL;
  { Scope* scope = NewScope(top_scope_, GLOBAL_SCOPE);
    info->SetGlobalScope(scope);
    if (!info->is_global()) {
      scope = Scope::DeserializeScopeChain(*info->calling_context(), scope);
      scope = NewScope(scope, EVAL_SCOPE);
    }
    scope->set_start_position(0);
    scope->set_end_position(source->length());
    FunctionState function_state(this, scope, isolate());
    top_scope_->SetLanguageMode(info->language_mode());
    ZoneList<Statement*>* body = new(zone()) ZoneList<Statement*>(16);
    bool ok = true;
    int beg_loc = scanner().location().beg_pos;
    ParseSourceElements(body, Token::EOS, &ok);
    if (ok && !top_scope_->is_classic_mode()) {
      CheckOctalLiteral(beg_loc, scanner().location().end_pos, &ok);
    }

    if (ok && is_extended_mode()) {
      CheckConflictingVarDeclarations(scope, &ok);
    }

    if (ok) {
      result = new(zone()) FunctionLiteral(
          isolate(),
          no_name,
          top_scope_,
          body,
          function_state.materialized_literal_count(),
          function_state.expected_property_count(),
          function_state.handler_count(),
          function_state.only_simple_this_property_assignments(),
          function_state.this_property_assignments(),
          0,
          FunctionLiteral::ANONYMOUS_EXPRESSION,
          false);  // Does not have duplicate parameters.
    } else if (stack_overflow_) {
      isolate()->StackOverflow();
    }
  }

  // Make sure the target stack is empty.
  ASSERT(target_stack_ == NULL);

  // If there was a syntax error we have to get rid of the AST
  // and it is not safe to do so before the scope has been deleted.
  if (result == NULL) zone_scope->DeleteOnExit();
  return result;
}

FunctionLiteral* Parser::ParseLazy(CompilationInfo* info) {
  ZoneScope zone_scope(isolate(), DONT_DELETE_ON_EXIT);
  HistogramTimerScope timer(isolate()->counters()->parse_lazy());
  Handle<String> source(String::cast(script_->source()));
  isolate()->counters()->total_parse_size()->Increment(source->length());

  Handle<SharedFunctionInfo> shared_info = info->shared_info();
  // Initialize parser state.
  source->TryFlatten();
  if (source->IsExternalTwoByteString()) {
    ExternalTwoByteStringUC16CharacterStream stream(
        Handle<ExternalTwoByteString>::cast(source),
        shared_info->start_position(),
        shared_info->end_position());
    FunctionLiteral* result = ParseLazy(info, &stream, &zone_scope);
    return result;
  } else {
    GenericStringUC16CharacterStream stream(source,
                                            shared_info->start_position(),
                                            shared_info->end_position());
    FunctionLiteral* result = ParseLazy(info, &stream, &zone_scope);
    return result;
  }
}


FunctionLiteral* Parser::ParseLazy(CompilationInfo* info,
                                   UC16CharacterStream* source,
                                   ZoneScope* zone_scope) {
  Handle<SharedFunctionInfo> shared_info = info->shared_info();
  scanner_.Initialize(source);
  ASSERT(top_scope_ == NULL);
  ASSERT(target_stack_ == NULL);

  Handle<String> name(String::cast(shared_info->name()));
  fni_ = new(zone()) FuncNameInferrer(isolate());
  fni_->PushEnclosingName(name);

  mode_ = PARSE_EAGERLY;

  // Place holder for the result.
  FunctionLiteral* result = NULL;

  {
    // Parse the function literal.
    Scope* scope = NewScope(top_scope_, GLOBAL_SCOPE);
    info->SetGlobalScope(scope);
    if (!info->closure().is_null()) {
      scope = Scope::DeserializeScopeChain(info->closure()->context(), scope);
    }
    FunctionState function_state(this, scope, isolate());
    ASSERT(scope->language_mode() != STRICT_MODE || !info->is_classic_mode());
    ASSERT(scope->language_mode() != EXTENDED_MODE ||
           info->is_extended_mode());
    ASSERT(info->language_mode() == shared_info->language_mode());
    scope->SetLanguageMode(shared_info->language_mode());
    FunctionLiteral::Type type = shared_info->is_expression()
        ? (shared_info->is_anonymous()
              ? FunctionLiteral::ANONYMOUS_EXPRESSION
              : FunctionLiteral::NAMED_EXPRESSION)
        : FunctionLiteral::DECLARATION;
    bool ok = true;
    result = ParseFunctionLiteral(name,
                                  false,  // Strict mode name already checked.
                                  RelocInfo::kNoPosition,
                                  type,
                                  &ok);
    // Make sure the results agree.
    ASSERT(ok == (result != NULL));
  }

  // Make sure the target stack is empty.
  ASSERT(target_stack_ == NULL);

  // If there was a stack overflow we have to get rid of AST and it is
  // not safe to do before scope has been deleted.
  if (result == NULL) {
    zone_scope->DeleteOnExit();
    if (stack_overflow_) isolate()->StackOverflow();
  } else {
    Handle<String> inferred_name(shared_info->inferred_name());
    result->set_inferred_name(inferred_name);
  }
  return result;
}


Handle<String> Parser::GetSymbol(bool* ok) {
  int symbol_id = -1;
  if (pre_data() != NULL) {
    symbol_id = pre_data()->GetSymbolIdentifier();
  }
  return LookupSymbol(symbol_id);
}


void Parser::ReportMessage(const char* type, Vector<const char*> args) {
  Scanner::Location source_location = scanner().location();
  ReportMessageAt(source_location, type, args);
}


void Parser::ReportMessageAt(Scanner::Location source_location,
                             const char* type,
                             Vector<const char*> args) {
  MessageLocation location(script_,
                           source_location.beg_pos,
                           source_location.end_pos);
  Factory* factory = isolate()->factory();
  Handle<FixedArray> elements = factory->NewFixedArray(args.length());
  for (int i = 0; i < args.length(); i++) {
    Handle<String> arg_string = factory->NewStringFromUtf8(CStrVector(args[i]));
    elements->set(i, *arg_string);
  }
  Handle<JSArray> array = factory->NewJSArrayWithElements(elements);
  Handle<Object> result = factory->NewSyntaxError(type, array);
  isolate()->Throw(*result, &location);
}


void Parser::ReportMessageAt(Scanner::Location source_location,
                             const char* type,
                             Vector<Handle<String> > args) {
  MessageLocation location(script_,
                           source_location.beg_pos,
                           source_location.end_pos);
  Factory* factory = isolate()->factory();
  Handle<FixedArray> elements = factory->NewFixedArray(args.length());
  for (int i = 0; i < args.length(); i++) {
    elements->set(i, *args[i]);
  }
  Handle<JSArray> array = factory->NewJSArrayWithElements(elements);
  Handle<Object> result = factory->NewSyntaxError(type, array);
  isolate()->Throw(*result, &location);
}


// Base class containing common code for the different finder classes used by
// the parser.
class ParserFinder {
 protected:
  ParserFinder() {}
  static Assignment* AsAssignment(Statement* stat) {
    if (stat == NULL) return NULL;
    ExpressionStatement* exp_stat = stat->AsExpressionStatement();
    if (exp_stat == NULL) return NULL;
    return exp_stat->expression()->AsAssignment();
  }
};


// An InitializationBlockFinder finds and marks sequences of statements of the
// form expr.a = ...; expr.b = ...; etc.
class InitializationBlockFinder : public ParserFinder {
 public:
  // We find and mark the initialization blocks in top level
  // non-looping code only. This is because the optimization prevents
  // reuse of the map transitions, so it should be used only for code
  // that will only be run once.
  InitializationBlockFinder(Scope* top_scope, Target* target)
      : enabled_(top_scope->DeclarationScope()->is_global_scope() &&
                 !IsLoopTarget(target)),
        first_in_block_(NULL),
        last_in_block_(NULL),
        block_size_(0) {}

  ~InitializationBlockFinder() {
    if (!enabled_) return;
    if (InBlock()) EndBlock();
  }

  void Update(Statement* stat) {
    if (!enabled_) return;
    Assignment* assignment = AsAssignment(stat);
    if (InBlock()) {
      if (BlockContinues(assignment)) {
        UpdateBlock(assignment);
      } else {
        EndBlock();
      }
    }
    if (!InBlock() && (assignment != NULL) &&
        (assignment->op() == Token::ASSIGN)) {
      StartBlock(assignment);
    }
  }

 private:
  // The minimum number of contiguous assignment that will
  // be treated as an initialization block. Benchmarks show that
  // the overhead exceeds the savings below this limit.
  static const int kMinInitializationBlock = 3;

  static bool IsLoopTarget(Target* target) {
    while (target != NULL) {
      if (target->node()->AsIterationStatement() != NULL) return true;
      target = target->previous();
    }
    return false;
  }

  // Returns true if the expressions appear to denote the same object.
  // In the context of initialization blocks, we only consider expressions
  // of the form 'expr.x' or expr["x"].
  static bool SameObject(Expression* e1, Expression* e2) {
    VariableProxy* v1 = e1->AsVariableProxy();
    VariableProxy* v2 = e2->AsVariableProxy();
    if (v1 != NULL && v2 != NULL) {
      return v1->name()->Equals(*v2->name());
    }
    Property* p1 = e1->AsProperty();
    Property* p2 = e2->AsProperty();
    if ((p1 == NULL) || (p2 == NULL)) return false;
    Literal* key1 = p1->key()->AsLiteral();
    Literal* key2 = p2->key()->AsLiteral();
    if ((key1 == NULL) || (key2 == NULL)) return false;
    if (!key1->handle()->IsString() || !key2->handle()->IsString()) {
      return false;
    }
    String* name1 = String::cast(*key1->handle());
    String* name2 = String::cast(*key2->handle());
    if (!name1->Equals(name2)) return false;
    return SameObject(p1->obj(), p2->obj());
  }

  // Returns true if the expressions appear to denote different properties
  // of the same object.
  static bool PropertyOfSameObject(Expression* e1, Expression* e2) {
    Property* p1 = e1->AsProperty();
    Property* p2 = e2->AsProperty();
    if ((p1 == NULL) || (p2 == NULL)) return false;
    return SameObject(p1->obj(), p2->obj());
  }

  bool BlockContinues(Assignment* assignment) {
    if ((assignment == NULL) || (first_in_block_ == NULL)) return false;
    if (assignment->op() != Token::ASSIGN) return false;
    return PropertyOfSameObject(first_in_block_->target(),
                                assignment->target());
  }

  void StartBlock(Assignment* assignment) {
    first_in_block_ = assignment;
    last_in_block_ = assignment;
    block_size_ = 1;
  }

  void UpdateBlock(Assignment* assignment) {
    last_in_block_ = assignment;
    ++block_size_;
  }

  void EndBlock() {
    if (block_size_ >= kMinInitializationBlock) {
      first_in_block_->mark_block_start();
      last_in_block_->mark_block_end();
    }
    last_in_block_ = first_in_block_ = NULL;
    block_size_ = 0;
  }

  bool InBlock() { return first_in_block_ != NULL; }

  const bool enabled_;
  Assignment* first_in_block_;
  Assignment* last_in_block_;
  int block_size_;

  DISALLOW_COPY_AND_ASSIGN(InitializationBlockFinder);
};


// A ThisNamedPropertyAssignmentFinder finds and marks statements of the form
// this.x = ...;, where x is a named property. It also determines whether a
// function contains only assignments of this type.
class ThisNamedPropertyAssignmentFinder : public ParserFinder {
 public:
  explicit ThisNamedPropertyAssignmentFinder(Isolate* isolate)
      : isolate_(isolate),
        only_simple_this_property_assignments_(true),
        names_(0),
        assigned_arguments_(0),
        assigned_constants_(0) {
  }

  void Update(Scope* scope, Statement* stat) {
    // Bail out if function already has property assignment that are
    // not simple this property assignments.
    if (!only_simple_this_property_assignments_) {
      return;
    }

    // Check whether this statement is of the form this.x = ...;
    Assignment* assignment = AsAssignment(stat);
    if (IsThisPropertyAssignment(assignment)) {
      HandleThisPropertyAssignment(scope, assignment);
    } else {
      only_simple_this_property_assignments_ = false;
    }
  }

  // Returns whether only statements of the form this.x = y; where y is either a
  // constant or a function argument was encountered.
  bool only_simple_this_property_assignments() {
    return only_simple_this_property_assignments_;
  }

  // Returns a fixed array containing three elements for each assignment of the
  // form this.x = y;
  Handle<FixedArray> GetThisPropertyAssignments() {
    if (names_.is_empty()) {
      return isolate_->factory()->empty_fixed_array();
    }
    ASSERT_EQ(names_.length(), assigned_arguments_.length());
    ASSERT_EQ(names_.length(), assigned_constants_.length());
    Handle<FixedArray> assignments =
        isolate_->factory()->NewFixedArray(names_.length() * 3);
    for (int i = 0; i < names_.length(); ++i) {
      assignments->set(i * 3, *names_[i]);
      assignments->set(i * 3 + 1, Smi::FromInt(assigned_arguments_[i]));
      assignments->set(i * 3 + 2, *assigned_constants_[i]);
    }
    return assignments;
  }

 private:
  bool IsThisPropertyAssignment(Assignment* assignment) {
    if (assignment != NULL) {
      Property* property = assignment->target()->AsProperty();
      return assignment->op() == Token::ASSIGN
             && property != NULL
             && property->obj()->AsVariableProxy() != NULL
             && property->obj()->AsVariableProxy()->is_this();
    }
    return false;
  }

  void HandleThisPropertyAssignment(Scope* scope, Assignment* assignment) {
    // Check that the property assigned to is a named property, which is not
    // __proto__.
    Property* property = assignment->target()->AsProperty();
    ASSERT(property != NULL);
    Literal* literal = property->key()->AsLiteral();
    uint32_t dummy;
    if (literal != NULL &&
        literal->handle()->IsString() &&
        !String::cast(*(literal->handle()))->Equals(
            isolate_->heap()->Proto_symbol()) &&
        !String::cast(*(literal->handle()))->AsArrayIndex(&dummy)) {
      Handle<String> key = Handle<String>::cast(literal->handle());

      // Check whether the value assigned is either a constant or matches the
      // name of one of the arguments to the function.
      if (assignment->value()->AsLiteral() != NULL) {
        // Constant assigned.
        Literal* literal = assignment->value()->AsLiteral();
        AssignmentFromConstant(key, literal->handle());
        return;
      } else if (assignment->value()->AsVariableProxy() != NULL) {
        // Variable assigned.
        Handle<String> name =
            assignment->value()->AsVariableProxy()->name();
        // Check whether the variable assigned matches an argument name.
        for (int i = 0; i < scope->num_parameters(); i++) {
          if (*scope->parameter(i)->name() == *name) {
            // Assigned from function argument.
            AssignmentFromParameter(key, i);
            return;
          }
        }
      }
    }
    // It is not a simple "this.x = value;" assignment with a constant
    // or parameter value.
    AssignmentFromSomethingElse();
  }




  // We will potentially reorder the property assignments, so they must be
  // simple enough that the ordering does not matter.
  void AssignmentFromParameter(Handle<String> name, int index) {
    EnsureInitialized();
    for (int i = 0; i < names_.length(); ++i) {
      if (name->Equals(*names_[i])) {
        assigned_arguments_[i] = index;
        assigned_constants_[i] = isolate_->factory()->undefined_value();
        return;
      }
    }
    names_.Add(name);
    assigned_arguments_.Add(index);
    assigned_constants_.Add(isolate_->factory()->undefined_value());
  }

  void AssignmentFromConstant(Handle<String> name, Handle<Object> value) {
    EnsureInitialized();
    for (int i = 0; i < names_.length(); ++i) {
      if (name->Equals(*names_[i])) {
        assigned_arguments_[i] = -1;
        assigned_constants_[i] = value;
        return;
      }
    }
    names_.Add(name);
    assigned_arguments_.Add(-1);
    assigned_constants_.Add(value);
  }

  void AssignmentFromSomethingElse() {
    // The this assignment is not a simple one.
    only_simple_this_property_assignments_ = false;
  }

  void EnsureInitialized() {
    if (names_.capacity() == 0) {
      ASSERT(assigned_arguments_.capacity() == 0);
      ASSERT(assigned_constants_.capacity() == 0);
      names_.Initialize(4);
      assigned_arguments_.Initialize(4);
      assigned_constants_.Initialize(4);
    }
  }

  Isolate* isolate_;
  bool only_simple_this_property_assignments_;
  ZoneStringList names_;
  ZoneList<int> assigned_arguments_;
  ZoneObjectList assigned_constants_;
};


Statement* Parser::ParseSourceElement(ZoneStringList* labels,
                                      bool* ok) {
  // (Ecma 262 5th Edition, clause 14):
  // SourceElement:
  //    Statement
  //    FunctionDeclaration
  //
  // In harmony mode we allow additionally the following productions
  // SourceElement:
  //    LetDeclaration
  //    ConstDeclaration

  if (peek() == Token::FUNCTION) {
    return ParseFunctionDeclaration(ok);
  } else if (peek() == Token::LET || peek() == Token::CONST) {
    return ParseVariableStatement(kSourceElement, ok);
  }
  return ParseStatement(labels, ok);
}


void* Parser::ParseSourceElements(ZoneList<Statement*>* processor,
                                  int end_token,
                                  bool* ok) {
  // SourceElements ::
  //   (SourceElement)* <end_token>

  // Allocate a target stack to use for this set of source
  // elements. This way, all scripts and functions get their own
  // target stack thus avoiding illegal breaks and continues across
  // functions.
  TargetScope scope(&this->target_stack_);

  ASSERT(processor != NULL);
  InitializationBlockFinder block_finder(top_scope_, target_stack_);
  ThisNamedPropertyAssignmentFinder this_property_assignment_finder(isolate());
  bool directive_prologue = true;     // Parsing directive prologue.

  while (peek() != end_token) {
    if (directive_prologue && peek() != Token::STRING) {
      directive_prologue = false;
    }

    Scanner::Location token_loc = scanner().peek_location();
    Statement* stat = ParseSourceElement(NULL, CHECK_OK);
    if (stat == NULL || stat->IsEmpty()) {
      directive_prologue = false;   // End of directive prologue.
      continue;
    }

    if (directive_prologue) {
      // A shot at a directive.
      ExpressionStatement *e_stat;
      Literal *literal;
      // Still processing directive prologue?
      if ((e_stat = stat->AsExpressionStatement()) != NULL &&
          (literal = e_stat->expression()->AsLiteral()) != NULL &&
          literal->handle()->IsString()) {
        Handle<String> directive = Handle<String>::cast(literal->handle());

        // Check "use strict" directive (ES5 14.1).
        if (top_scope_->is_classic_mode() &&
            directive->Equals(isolate()->heap()->use_strict()) &&
            token_loc.end_pos - token_loc.beg_pos ==
              isolate()->heap()->use_strict()->length() + 2) {
          // TODO(ES6): Fix entering extended mode, once it is specified.
          top_scope_->SetLanguageMode(FLAG_harmony_scoping
                                      ? EXTENDED_MODE : STRICT_MODE);
          // "use strict" is the only directive for now.
          directive_prologue = false;
        }
      } else {
        // End of the directive prologue.
        directive_prologue = false;
      }
    }

    block_finder.Update(stat);
    // Find and mark all assignments to named properties in this (this.x =)
    if (top_scope_->is_function_scope()) {
      this_property_assignment_finder.Update(top_scope_, stat);
    }
    processor->Add(stat);
  }

  // Propagate the collected information on this property assignments.
  if (top_scope_->is_function_scope()) {
    bool only_simple_this_property_assignments =
        this_property_assignment_finder.only_simple_this_property_assignments()
        && top_scope_->declarations()->length() == 0;
    if (only_simple_this_property_assignments) {
      current_function_state_->SetThisPropertyAssignmentInfo(
          only_simple_this_property_assignments,
          this_property_assignment_finder.GetThisPropertyAssignments());
    }
  }
  return 0;
}


Statement* Parser::ParseStatement(ZoneStringList* labels, bool* ok) {
  // Statement ::
  //   Block
  //   VariableStatement
  //   EmptyStatement
  //   ExpressionStatement
  //   IfStatement
  //   IterationStatement
  //   ContinueStatement
  //   BreakStatement
  //   ReturnStatement
  //   WithStatement
  //   LabelledStatement
  //   SwitchStatement
  //   ThrowStatement
  //   TryStatement
  //   DebuggerStatement

  // Note: Since labels can only be used by 'break' and 'continue'
  // statements, which themselves are only valid within blocks,
  // iterations or 'switch' statements (i.e., BreakableStatements),
  // labels can be simply ignored in all other cases; except for
  // trivial labeled break statements 'label: break label' which is
  // parsed into an empty statement.

  // Keep the source position of the statement
  int statement_pos = scanner().peek_location().beg_pos;
  Statement* stmt = NULL;
  switch (peek()) {
    case Token::LBRACE:
      return ParseBlock(labels, ok);

    case Token::CONST:  // fall through
    case Token::LET:
    case Token::VAR:
      stmt = ParseVariableStatement(kStatement, ok);
      break;

    case Token::SEMICOLON:
      Next();
      return EmptyStatement();

    case Token::IF:
      stmt = ParseIfStatement(labels, ok);
      break;

    case Token::DO:
      stmt = ParseDoWhileStatement(labels, ok);
      break;

    case Token::WHILE:
      stmt = ParseWhileStatement(labels, ok);
      break;

    case Token::FOR:
      stmt = ParseForStatement(labels, ok);
      break;

    case Token::CONTINUE:
      stmt = ParseContinueStatement(ok);
      break;

    case Token::BREAK:
      stmt = ParseBreakStatement(labels, ok);
      break;

    case Token::RETURN:
      stmt = ParseReturnStatement(ok);
      break;

    case Token::WITH:
      stmt = ParseWithStatement(labels, ok);
      break;

    case Token::SWITCH:
      stmt = ParseSwitchStatement(labels, ok);
      break;

    case Token::THROW:
      stmt = ParseThrowStatement(ok);
      break;

    case Token::TRY: {
      // NOTE: It is somewhat complicated to have labels on
      // try-statements. When breaking out of a try-finally statement,
      // one must take great care not to treat it as a
      // fall-through. It is much easier just to wrap the entire
      // try-statement in a statement block and put the labels there
      Block* result = new(zone()) Block(isolate(), labels, 1, false);
      Target target(&this->target_stack_, result);
      TryStatement* statement = ParseTryStatement(CHECK_OK);
      if (statement) {
        statement->set_statement_pos(statement_pos);
      }
      if (result) result->AddStatement(statement);
      return result;
    }

    case Token::FUNCTION: {
      // FunctionDeclaration is only allowed in the context of SourceElements
      // (Ecma 262 5th Edition, clause 14):
      // SourceElement:
      //    Statement
      //    FunctionDeclaration
      // Common language extension is to allow function declaration in place
      // of any statement. This language extension is disabled in strict mode.
      if (!top_scope_->is_classic_mode()) {
        ReportMessageAt(scanner().peek_location(), "strict_function",
                        Vector<const char*>::empty());
        *ok = false;
        return NULL;
      }
      return ParseFunctionDeclaration(ok);
    }

    case Token::DEBUGGER:
      stmt = ParseDebuggerStatement(ok);
      break;

    default:
      stmt = ParseExpressionOrLabelledStatement(labels, ok);
  }

  // Store the source position of the statement
  if (stmt != NULL) stmt->set_statement_pos(statement_pos);
  return stmt;
}


VariableProxy* Parser::Declare(Handle<String> name,
                               VariableMode mode,
                               FunctionLiteral* fun,
                               bool resolve,
                               bool* ok) {
  Variable* var = NULL;
  // If we are inside a function, a declaration of a var/const variable is a
  // truly local variable, and the scope of the variable is always the function
  // scope.
  // Let/const variables in harmony mode are always added to the immediately
  // enclosing scope.
  Scope* declaration_scope = (mode == LET || mode == CONST_HARMONY)
      ? top_scope_ : top_scope_->DeclarationScope();
  InitializationFlag init_flag = (fun != NULL || mode == VAR)
      ? kCreatedInitialized : kNeedsInitialization;

  // If a function scope exists, then we can statically declare this
  // variable and also set its mode. In any case, a Declaration node
  // will be added to the scope so that the declaration can be added
  // to the corresponding activation frame at runtime if necessary.
  // For instance declarations inside an eval scope need to be added
  // to the calling function context.
  // Similarly, strict mode eval scope does not leak variable declarations to
  // the caller's scope so we declare all locals, too.
  // Also for block scoped let/const bindings the variable can be
  // statically declared.
  if (declaration_scope->is_function_scope() ||
      declaration_scope->is_strict_or_extended_eval_scope() ||
      declaration_scope->is_block_scope()) {
    // Declare the variable in the function scope.
    var = declaration_scope->LocalLookup(name);
    if (var == NULL) {
      // Declare the name.
      var = declaration_scope->DeclareLocal(name, mode, init_flag);
    } else {
      // The name was declared in this scope before; check for conflicting
      // re-declarations. We have a conflict if either of the declarations is
      // not a var. There is similar code in runtime.cc in the Declare
      // functions. The function CheckNonConflictingScope checks for conflicting
      // var and let bindings from different scopes whereas this is a check for
      // conflicting declarations within the same scope. This check also covers
      //
      // function () { let x; { var x; } }
      //
      // because the var declaration is hoisted to the function scope where 'x'
      // is already bound.
      if ((mode != VAR) || (var->mode() != VAR)) {
        // We only have vars, consts and lets in declarations.
        ASSERT(var->mode() == VAR ||
               var->mode() == CONST ||
               var->mode() == CONST_HARMONY ||
               var->mode() == LET);
        if (is_extended_mode()) {
          // In harmony mode we treat re-declarations as early errors. See
          // ES5 16 for a definition of early errors.
          SmartArrayPointer<char> c_string = name->ToCString(DISALLOW_NULLS);
          const char* elms[2] = { "Variable", *c_string };
          Vector<const char*> args(elms, 2);
          ReportMessage("redeclaration", args);
          *ok = false;
          return NULL;
        }
        const char* type = (var->mode() == VAR)
            ? "var" : var->is_const_mode() ? "const" : "let";
        Handle<String> type_string =
            isolate()->factory()->NewStringFromUtf8(CStrVector(type), TENURED);
        Expression* expression =
            NewThrowTypeError(isolate()->factory()->redeclaration_symbol(),
                              type_string, name);
        declaration_scope->SetIllegalRedeclaration(expression);
      }
    }
  }

  // We add a declaration node for every declaration. The compiler
  // will only generate code if necessary. In particular, declarations
  // for inner local variables that do not represent functions won't
  // result in any generated code.
  //
  // Note that we always add an unresolved proxy even if it's not
  // used, simply because we don't know in this method (w/o extra
  // parameters) if the proxy is needed or not. The proxy will be
  // bound during variable resolution time unless it was pre-bound
  // below.
  //
  // WARNING: This will lead to multiple declaration nodes for the
  // same variable if it is declared several times. This is not a
  // semantic issue as long as we keep the source order, but it may be
  // a performance issue since it may lead to repeated
  // Runtime::DeclareContextSlot() calls.
  VariableProxy* proxy = declaration_scope->NewUnresolved(
      name, scanner().location().beg_pos);
  declaration_scope->AddDeclaration(
      new(zone()) Declaration(proxy, mode, fun, top_scope_));

  if ((mode == CONST || mode == CONST_HARMONY) &&
      declaration_scope->is_global_scope()) {
    // For global const variables we bind the proxy to a variable.
    ASSERT(resolve);  // should be set by all callers
    Variable::Kind kind = Variable::NORMAL;
    var = new(zone()) Variable(declaration_scope,
                               name,
                               mode,
                               true,
                               kind,
                               kNeedsInitialization);
  } else if (declaration_scope->is_eval_scope() &&
             declaration_scope->is_classic_mode()) {
    // For variable declarations in a non-strict eval scope the proxy is bound
    // to a lookup variable to force a dynamic declaration using the
    // DeclareContextSlot runtime function.
    Variable::Kind kind = Variable::NORMAL;
    var = new(zone()) Variable(declaration_scope,
                               name,
                               mode,
                               true,
                               kind,
                               init_flag);
    var->AllocateTo(Variable::LOOKUP, -1);
    resolve = true;
  }

  // If requested and we have a local variable, bind the proxy to the variable
  // at parse-time. This is used for functions (and consts) declared inside
  // statements: the corresponding function (or const) variable must be in the
  // function scope and not a statement-local scope, e.g. as provided with a
  // 'with' statement:
  //
  //   with (obj) {
  //     function f() {}
  //   }
  //
  // which is translated into:
  //
  //   with (obj) {
  //     // in this case this is not: 'var f; f = function () {};'
  //     var f = function () {};
  //   }
  //
  // Note that if 'f' is accessed from inside the 'with' statement, it
  // will be allocated in the context (because we must be able to look
  // it up dynamically) but it will also be accessed statically, i.e.,
  // with a context slot index and a context chain length for this
  // initialization code. Thus, inside the 'with' statement, we need
  // both access to the static and the dynamic context chain; the
  // runtime needs to provide both.
  if (resolve && var != NULL) proxy->BindTo(var);

  return proxy;
}


// Language extension which is only enabled for source files loaded
// through the API's extension mechanism.  A native function
// declaration is resolved by looking up the function through a
// callback provided by the extension.
Statement* Parser::ParseNativeDeclaration(bool* ok) {
  Expect(Token::FUNCTION, CHECK_OK);
  Handle<String> name = ParseIdentifier(CHECK_OK);
  Expect(Token::LPAREN, CHECK_OK);
  bool done = (peek() == Token::RPAREN);
  while (!done) {
    ParseIdentifier(CHECK_OK);
    done = (peek() == Token::RPAREN);
    if (!done) {
      Expect(Token::COMMA, CHECK_OK);
    }
  }
  Expect(Token::RPAREN, CHECK_OK);
  Expect(Token::SEMICOLON, CHECK_OK);

  // Make sure that the function containing the native declaration
  // isn't lazily compiled. The extension structures are only
  // accessible while parsing the first time not when reparsing
  // because of lazy compilation.
  top_scope_->DeclarationScope()->ForceEagerCompilation();

  // Compute the function template for the native function.
  v8::Handle<v8::FunctionTemplate> fun_template =
      extension_->GetNativeFunction(v8::Utils::ToLocal(name));
  ASSERT(!fun_template.IsEmpty());

  // Instantiate the function and create a shared function info from it.
  Handle<JSFunction> fun = Utils::OpenHandle(*fun_template->GetFunction());
  const int literals = fun->NumberOfLiterals();
  Handle<Code> code = Handle<Code>(fun->shared()->code());
  Handle<Code> construct_stub = Handle<Code>(fun->shared()->construct_stub());
  Handle<SharedFunctionInfo> shared =
      isolate()->factory()->NewSharedFunctionInfo(name, literals, code,
          Handle<ScopeInfo>(fun->shared()->scope_info()));
  shared->set_construct_stub(*construct_stub);

  // Copy the function data to the shared function info.
  shared->set_function_data(fun->shared()->function_data());
  int parameters = fun->shared()->formal_parameter_count();
  shared->set_formal_parameter_count(parameters);

  // TODO(1240846): It's weird that native function declarations are
  // introduced dynamically when we meet their declarations, whereas
  // other functions are setup when entering the surrounding scope.
  SharedFunctionInfoLiteral* lit =
      new(zone()) SharedFunctionInfoLiteral(isolate(), shared);
  VariableProxy* var = Declare(name, VAR, NULL, true, CHECK_OK);
  return new(zone()) ExpressionStatement(new(zone()) Assignment(
      isolate(), Token::INIT_VAR, var, lit, RelocInfo::kNoPosition));
}


Statement* Parser::ParseFunctionDeclaration(bool* ok) {
  // FunctionDeclaration ::
  //   'function' Identifier '(' FormalParameterListopt ')' '{' FunctionBody '}'
  Expect(Token::FUNCTION, CHECK_OK);
  int function_token_position = scanner().location().beg_pos;
  bool is_strict_reserved = false;
  Handle<String> name = ParseIdentifierOrStrictReservedWord(
      &is_strict_reserved, CHECK_OK);
  FunctionLiteral* fun = ParseFunctionLiteral(name,
                                              is_strict_reserved,
                                              function_token_position,
                                              FunctionLiteral::DECLARATION,
                                              CHECK_OK);
  // Even if we're not at the top-level of the global or a function
  // scope, we treat is as such and introduce the function with it's
  // initial value upon entering the corresponding scope.
  VariableMode mode = is_extended_mode() ? LET : VAR;
  Declare(name, mode, fun, true, CHECK_OK);
  return EmptyStatement();
}


Block* Parser::ParseBlock(ZoneStringList* labels, bool* ok) {
  if (top_scope_->is_extended_mode()) return ParseScopedBlock(labels, ok);

  // Block ::
  //   '{' Statement* '}'

  // Note that a Block does not introduce a new execution scope!
  // (ECMA-262, 3rd, 12.2)
  //
  // Construct block expecting 16 statements.
  Block* result = new(zone()) Block(isolate(), labels, 16, false);
  Target target(&this->target_stack_, result);
  Expect(Token::LBRACE, CHECK_OK);
  InitializationBlockFinder block_finder(top_scope_, target_stack_);
  while (peek() != Token::RBRACE) {
    Statement* stat = ParseStatement(NULL, CHECK_OK);
    if (stat && !stat->IsEmpty()) {
      result->AddStatement(stat);
      block_finder.Update(stat);
    }
  }
  Expect(Token::RBRACE, CHECK_OK);
  return result;
}


Block* Parser::ParseScopedBlock(ZoneStringList* labels, bool* ok) {
  // The harmony mode uses source elements instead of statements.
  //
  // Block ::
  //   '{' SourceElement* '}'

  // Construct block expecting 16 statements.
  Block* body = new(zone()) Block(isolate(), labels, 16, false);
  Scope* block_scope = NewScope(top_scope_, BLOCK_SCOPE);

  // Parse the statements and collect escaping labels.
  Expect(Token::LBRACE, CHECK_OK);
  block_scope->set_start_position(scanner().location().beg_pos);
  { BlockState block_state(this, block_scope);
    TargetCollector collector;
    Target target(&this->target_stack_, &collector);
    Target target_body(&this->target_stack_, body);
    InitializationBlockFinder block_finder(top_scope_, target_stack_);

    while (peek() != Token::RBRACE) {
      Statement* stat = ParseSourceElement(NULL, CHECK_OK);
      if (stat && !stat->IsEmpty()) {
        body->AddStatement(stat);
        block_finder.Update(stat);
      }
    }
  }
  Expect(Token::RBRACE, CHECK_OK);
  block_scope->set_end_position(scanner().location().end_pos);
  block_scope = block_scope->FinalizeBlockScope();
  body->set_block_scope(block_scope);
  return body;
}


Block* Parser::ParseVariableStatement(VariableDeclarationContext var_context,
                                      bool* ok) {
  // VariableStatement ::
  //   VariableDeclarations ';'

  Handle<String> ignore;
  Block* result = ParseVariableDeclarations(var_context,
                                            NULL,
                                            &ignore,
                                            CHECK_OK);
  ExpectSemicolon(CHECK_OK);
  return result;
}


bool Parser::IsEvalOrArguments(Handle<String> string) {
  return string.is_identical_to(isolate()->factory()->eval_symbol()) ||
      string.is_identical_to(isolate()->factory()->arguments_symbol());
}


// If the variable declaration declares exactly one non-const
// variable, then *var is set to that variable. In all other cases,
// *var is untouched; in particular, it is the caller's responsibility
// to initialize it properly. This mechanism is used for the parsing
// of 'for-in' loops.
Block* Parser::ParseVariableDeclarations(
    VariableDeclarationContext var_context,
    VariableDeclarationProperties* decl_props,
    Handle<String>* out,
    bool* ok) {
  // VariableDeclarations ::
  //   ('var' | 'const' | 'let') (Identifier ('=' AssignmentExpression)?)+[',']
  //
  // The ES6 Draft Rev3 specifies the following grammar for const declarations
  //
  // ConstDeclaration ::
  //   const ConstBinding (',' ConstBinding)* ';'
  // ConstBinding ::
  //   Identifier '=' AssignmentExpression
  //
  // TODO(ES6):
  // ConstBinding ::
  //   BindingPattern '=' AssignmentExpression
  VariableMode mode = VAR;
  // True if the binding needs initialization. 'let' and 'const' declared
  // bindings are created uninitialized by their declaration nodes and
  // need initialization. 'var' declared bindings are always initialized
  // immediately by their declaration nodes.
  bool needs_init = false;
  bool is_const = false;
  Token::Value init_op = Token::INIT_VAR;
  if (peek() == Token::VAR) {
    Consume(Token::VAR);
  } else if (peek() == Token::CONST) {
    // TODO(ES6): The ES6 Draft Rev4 section 12.2.2 reads:
    //
    // ConstDeclaration : const ConstBinding (',' ConstBinding)* ';'
    //
    // * It is a Syntax Error if the code that matches this production is not
    //   contained in extended code.
    //
    // However disallowing const in classic mode will break compatibility with
    // existing pages. Therefore we keep allowing const with the old
    // non-harmony semantics in classic mode.
    Consume(Token::CONST);
    switch (top_scope_->language_mode()) {
      case CLASSIC_MODE:
        mode = CONST;
        init_op = Token::INIT_CONST;
        break;
      case STRICT_MODE:
        ReportMessage("strict_const", Vector<const char*>::empty());
        *ok = false;
        return NULL;
      case EXTENDED_MODE:
        if (var_context != kSourceElement &&
            var_context != kForStatement) {
          // In extended mode 'const' declarations are only allowed in source
          // element positions.
          ReportMessage("unprotected_const", Vector<const char*>::empty());
          *ok = false;
          return NULL;
        }
        mode = CONST_HARMONY;
        init_op = Token::INIT_CONST_HARMONY;
    }
    is_const = true;
    needs_init = true;
  } else if (peek() == Token::LET) {
    // ES6 Draft Rev4 section 12.2.1:
    //
    // LetDeclaration : let LetBindingList ;
    //
    // * It is a Syntax Error if the code that matches this production is not
    //   contained in extended code.
    if (!is_extended_mode()) {
      ReportMessage("illegal_let", Vector<const char*>::empty());
      *ok = false;
      return NULL;
    }
    Consume(Token::LET);
    if (var_context != kSourceElement &&
        var_context != kForStatement) {
      // Let declarations are only allowed in source element positions.
      ASSERT(var_context == kStatement);
      ReportMessage("unprotected_let", Vector<const char*>::empty());
      *ok = false;
      return NULL;
    }
    mode = LET;
    needs_init = true;
    init_op = Token::INIT_LET;
  } else {
    UNREACHABLE();  // by current callers
  }

  Scope* declaration_scope = (mode == LET || mode == CONST_HARMONY)
      ? top_scope_ : top_scope_->DeclarationScope();
  // The scope of a var/const declared variable anywhere inside a function
  // is the entire function (ECMA-262, 3rd, 10.1.3, and 12.2). Thus we can
  // transform a source-level var/const declaration into a (Function)
  // Scope declaration, and rewrite the source-level initialization into an
  // assignment statement. We use a block to collect multiple assignments.
  //
  // We mark the block as initializer block because we don't want the
  // rewriter to add a '.result' assignment to such a block (to get compliant
  // behavior for code such as print(eval('var x = 7')), and for cosmetic
  // reasons when pretty-printing. Also, unless an assignment (initialization)
  // is inside an initializer block, it is ignored.
  //
  // Create new block with one expected declaration.
  Block* block = new(zone()) Block(isolate(), NULL, 1, true);
  int nvars = 0;  // the number of variables declared
  Handle<String> name;
  do {
    if (fni_ != NULL) fni_->Enter();

    // Parse variable name.
    if (nvars > 0) Consume(Token::COMMA);
    name = ParseIdentifier(CHECK_OK);
    if (fni_ != NULL) fni_->PushVariableName(name);

    // Strict mode variables may not be named eval or arguments
    if (!declaration_scope->is_classic_mode() && IsEvalOrArguments(name)) {
      ReportMessage("strict_var_name", Vector<const char*>::empty());
      *ok = false;
      return NULL;
    }

    // Declare variable.
    // Note that we *always* must treat the initial value via a separate init
    // assignment for variables and constants because the value must be assigned
    // when the variable is encountered in the source. But the variable/constant
    // is declared (and set to 'undefined') upon entering the function within
    // which the variable or constant is declared. Only function variables have
    // an initial value in the declaration (because they are initialized upon
    // entering the function).
    //
    // If we have a const declaration, in an inner scope, the proxy is always
    // bound to the declared variable (independent of possibly surrounding with
    // statements).
    // For let/const declarations in harmony mode, we can also immediately
    // pre-resolve the proxy because it resides in the same scope as the
    // declaration.
    VariableProxy* proxy = Declare(name, mode, NULL, mode != VAR, CHECK_OK);
    nvars++;
    if (declaration_scope->num_var_or_const() > kMaxNumFunctionLocals) {
      ReportMessageAt(scanner().location(), "too_many_variables",
                      Vector<const char*>::empty());
      *ok = false;
      return NULL;
    }

    // Parse initialization expression if present and/or needed. A
    // declaration of the form:
    //
    //    var v = x;
    //
    // is syntactic sugar for:
    //
    //    var v; v = x;
    //
    // In particular, we need to re-lookup 'v' (in top_scope_, not
    // declaration_scope) as it may be a different 'v' than the 'v' in the
    // declaration (e.g., if we are inside a 'with' statement or 'catch'
    // block).
    //
    // However, note that const declarations are different! A const
    // declaration of the form:
    //
    //   const c = x;
    //
    // is *not* syntactic sugar for:
    //
    //   const c; c = x;
    //
    // The "variable" c initialized to x is the same as the declared
    // one - there is no re-lookup (see the last parameter of the
    // Declare() call above).

    Scope* initialization_scope = is_const ? declaration_scope : top_scope_;
    Expression* value = NULL;
    int position = -1;
    // Harmony consts have non-optional initializers.
    if (peek() == Token::ASSIGN || mode == CONST_HARMONY) {
      Expect(Token::ASSIGN, CHECK_OK);
      position = scanner().location().beg_pos;
      value = ParseAssignmentExpression(var_context != kForStatement, CHECK_OK);
      // Don't infer if it is "a = function(){...}();"-like expression.
      if (fni_ != NULL &&
          value->AsCall() == NULL &&
          value->AsCallNew() == NULL) {
        fni_->Infer();
      } else {
        fni_->RemoveLastFunction();
      }
      if (decl_props != NULL) *decl_props = kHasInitializers;
    }

    // Record the end position of the initializer.
    if (proxy->var() != NULL) {
      proxy->var()->set_initializer_position(scanner().location().end_pos);
    }

    // Make sure that 'const x' and 'let x' initialize 'x' to undefined.
    if (value == NULL && needs_init) {
      value = GetLiteralUndefined();
    }

    // Global variable declarations must be compiled in a specific
    // way. When the script containing the global variable declaration
    // is entered, the global variable must be declared, so that if it
    // doesn't exist (not even in a prototype of the global object) it
    // gets created with an initial undefined value. This is handled
    // by the declarations part of the function representing the
    // top-level global code; see Runtime::DeclareGlobalVariable. If
    // it already exists (in the object or in a prototype), it is
    // *not* touched until the variable declaration statement is
    // executed.
    //
    // Executing the variable declaration statement will always
    // guarantee to give the global object a "local" variable; a
    // variable defined in the global object and not in any
    // prototype. This way, global variable declarations can shadow
    // properties in the prototype chain, but only after the variable
    // declaration statement has been executed. This is important in
    // browsers where the global object (window) has lots of
    // properties defined in prototype objects.
    if (initialization_scope->is_global_scope()) {
      // Compute the arguments for the runtime call.
      ZoneList<Expression*>* arguments = new(zone()) ZoneList<Expression*>(3);
      // We have at least 1 parameter.
      arguments->Add(NewLiteral(name));
      CallRuntime* initialize;

      if (is_const) {
        arguments->Add(value);
        value = NULL;  // zap the value to avoid the unnecessary assignment

        // Construct the call to Runtime_InitializeConstGlobal
        // and add it to the initialization statement block.
        // Note that the function does different things depending on
        // the number of arguments (1 or 2).
        initialize =
            new(zone()) CallRuntime(
                isolate(),
                isolate()->factory()->InitializeConstGlobal_symbol(),
                Runtime::FunctionForId(Runtime::kInitializeConstGlobal),
                arguments);
      } else {
        // Add strict mode.
        // We may want to pass singleton to avoid Literal allocations.
        LanguageMode language_mode = initialization_scope->language_mode();
        arguments->Add(NewNumberLiteral(language_mode));

        // Be careful not to assign a value to the global variable if
        // we're in a with. The initialization value should not
        // necessarily be stored in the global object in that case,
        // which is why we need to generate a separate assignment node.
        if (value != NULL && !inside_with()) {
          arguments->Add(value);
          value = NULL;  // zap the value to avoid the unnecessary assignment
        }

        // Construct the call to Runtime_InitializeVarGlobal
        // and add it to the initialization statement block.
        // Note that the function does different things depending on
        // the number of arguments (2 or 3).
        initialize =
            new(zone()) CallRuntime(
                isolate(),
                isolate()->factory()->InitializeVarGlobal_symbol(),
                Runtime::FunctionForId(Runtime::kInitializeVarGlobal),
                arguments);
      }

      block->AddStatement(new(zone()) ExpressionStatement(initialize));
    } else if (needs_init) {
      // Constant initializations always assign to the declared constant which
      // is always at the function scope level. This is only relevant for
      // dynamically looked-up variables and constants (the start context for
      // constant lookups is always the function context, while it is the top
      // context for var declared variables). Sigh...
      // For 'let' and 'const' declared variables in harmony mode the
      // initialization also always assigns to the declared variable.
      ASSERT(proxy != NULL);
      ASSERT(proxy->var() != NULL);
      ASSERT(value != NULL);
      Assignment* assignment =
          new(zone()) Assignment(isolate(), init_op, proxy, value, position);
      block->AddStatement(new(zone()) ExpressionStatement(assignment));
      value = NULL;
    }

    // Add an assignment node to the initialization statement block if we still
    // have a pending initialization value.
    if (value != NULL) {
      ASSERT(mode == VAR);
      // 'var' initializations are simply assignments (with all the consequences
      // if they are inside a 'with' statement - they may change a 'with' object
      // property).
      VariableProxy* proxy = initialization_scope->NewUnresolved(name);
      Assignment* assignment =
          new(zone()) Assignment(isolate(), init_op, proxy, value, position);
      block->AddStatement(new(zone()) ExpressionStatement(assignment));
    }

    if (fni_ != NULL) fni_->Leave();
  } while (peek() == Token::COMMA);

  // If there was a single non-const declaration, return it in the output
  // parameter for possible use by for/in.
  if (nvars == 1 && !is_const) {
    *out = name;
  }

  return block;
}


static bool ContainsLabel(ZoneStringList* labels, Handle<String> label) {
  ASSERT(!label.is_null());
  if (labels != NULL)
    for (int i = labels->length(); i-- > 0; )
      if (labels->at(i).is_identical_to(label))
        return true;

  return false;
}


Statement* Parser::ParseExpressionOrLabelledStatement(ZoneStringList* labels,
                                                      bool* ok) {
  // ExpressionStatement | LabelledStatement ::
  //   Expression ';'
  //   Identifier ':' Statement
  bool starts_with_idenfifier = peek_any_identifier();
  Expression* expr = ParseExpression(true, CHECK_OK);
  if (peek() == Token::COLON && starts_with_idenfifier && expr != NULL &&
      expr->AsVariableProxy() != NULL &&
      !expr->AsVariableProxy()->is_this()) {
    // Expression is a single identifier, and not, e.g., a parenthesized
    // identifier.
    VariableProxy* var = expr->AsVariableProxy();
    Handle<String> label = var->name();
    // TODO(1240780): We don't check for redeclaration of labels
    // during preparsing since keeping track of the set of active
    // labels requires nontrivial changes to the way scopes are
    // structured.  However, these are probably changes we want to
    // make later anyway so we should go back and fix this then.
    if (ContainsLabel(labels, label) || TargetStackContainsLabel(label)) {
      SmartArrayPointer<char> c_string = label->ToCString(DISALLOW_NULLS);
      const char* elms[2] = { "Label", *c_string };
      Vector<const char*> args(elms, 2);
      ReportMessage("redeclaration", args);
      *ok = false;
      return NULL;
    }
    if (labels == NULL) labels = new(zone()) ZoneStringList(4);
    labels->Add(label);
    // Remove the "ghost" variable that turned out to be a label
    // from the top scope. This way, we don't try to resolve it
    // during the scope processing.
    top_scope_->RemoveUnresolved(var);
    Expect(Token::COLON, CHECK_OK);
    return ParseStatement(labels, ok);
  }

  // If we have an extension, we allow a native function declaration.
  // A native function declaration starts with "native function" with
  // no line-terminator between the two words.
  if (extension_ != NULL &&
      peek() == Token::FUNCTION &&
      !scanner().HasAnyLineTerminatorBeforeNext() &&
      expr != NULL &&
      expr->AsVariableProxy() != NULL &&
      expr->AsVariableProxy()->name()->Equals(
          isolate()->heap()->native_symbol()) &&
      !scanner().literal_contains_escapes()) {
    return ParseNativeDeclaration(ok);
  }

  // Parsed expression statement.
  ExpectSemicolon(CHECK_OK);
  return new(zone()) ExpressionStatement(expr);
}


IfStatement* Parser::ParseIfStatement(ZoneStringList* labels, bool* ok) {
  // IfStatement ::
  //   'if' '(' Expression ')' Statement ('else' Statement)?

  Expect(Token::IF, CHECK_OK);
  Expect(Token::LPAREN, CHECK_OK);
  Expression* condition = ParseExpression(true, CHECK_OK);
  Expect(Token::RPAREN, CHECK_OK);
  Statement* then_statement = ParseStatement(labels, CHECK_OK);
  Statement* else_statement = NULL;
  if (peek() == Token::ELSE) {
    Next();
    else_statement = ParseStatement(labels, CHECK_OK);
  } else {
    else_statement = EmptyStatement();
  }
  return new(zone()) IfStatement(
      isolate(), condition, then_statement, else_statement);
}


Statement* Parser::ParseContinueStatement(bool* ok) {
  // ContinueStatement ::
  //   'continue' Identifier? ';'

  Expect(Token::CONTINUE, CHECK_OK);
  Handle<String> label = Handle<String>::null();
  Token::Value tok = peek();
  if (!scanner().HasAnyLineTerminatorBeforeNext() &&
      tok != Token::SEMICOLON && tok != Token::RBRACE && tok != Token::EOS) {
    label = ParseIdentifier(CHECK_OK);
  }
  IterationStatement* target = NULL;
  target = LookupContinueTarget(label, CHECK_OK);
  if (target == NULL) {
    // Illegal continue statement.
    const char* message = "illegal_continue";
    Vector<Handle<String> > args;
    if (!label.is_null()) {
      message = "unknown_label";
      args = Vector<Handle<String> >(&label, 1);
    }
    ReportMessageAt(scanner().location(), message, args);
    *ok = false;
    return NULL;
  }
  ExpectSemicolon(CHECK_OK);
  return new(zone()) ContinueStatement(target);
}


Statement* Parser::ParseBreakStatement(ZoneStringList* labels, bool* ok) {
  // BreakStatement ::
  //   'break' Identifier? ';'

  Expect(Token::BREAK, CHECK_OK);
  Handle<String> label;
  Token::Value tok = peek();
  if (!scanner().HasAnyLineTerminatorBeforeNext() &&
      tok != Token::SEMICOLON && tok != Token::RBRACE && tok != Token::EOS) {
    label = ParseIdentifier(CHECK_OK);
  }
  // Parse labeled break statements that target themselves into
  // empty statements, e.g. 'l1: l2: l3: break l2;'
  if (!label.is_null() && ContainsLabel(labels, label)) {
    return EmptyStatement();
  }
  BreakableStatement* target = NULL;
  target = LookupBreakTarget(label, CHECK_OK);
  if (target == NULL) {
    // Illegal break statement.
    const char* message = "illegal_break";
    Vector<Handle<String> > args;
    if (!label.is_null()) {
      message = "unknown_label";
      args = Vector<Handle<String> >(&label, 1);
    }
    ReportMessageAt(scanner().location(), message, args);
    *ok = false;
    return NULL;
  }
  ExpectSemicolon(CHECK_OK);
  return new(zone()) BreakStatement(target);
}


Statement* Parser::ParseReturnStatement(bool* ok) {
  // ReturnStatement ::
  //   'return' Expression? ';'

  // Consume the return token. It is necessary to do the before
  // reporting any errors on it, because of the way errors are
  // reported (underlining).
  Expect(Token::RETURN, CHECK_OK);

  // An ECMAScript program is considered syntactically incorrect if it
  // contains a return statement that is not within the body of a
  // function. See ECMA-262, section 12.9, page 67.
  //
  // To be consistent with KJS we report the syntax error at runtime.
  Scope* declaration_scope = top_scope_->DeclarationScope();
  if (declaration_scope->is_global_scope() ||
      declaration_scope->is_eval_scope()) {
    Handle<String> type = isolate()->factory()->illegal_return_symbol();
    Expression* throw_error = NewThrowSyntaxError(type, Handle<Object>::null());
    return new(zone()) ExpressionStatement(throw_error);
  }

  Token::Value tok = peek();
  if (scanner().HasAnyLineTerminatorBeforeNext() ||
      tok == Token::SEMICOLON ||
      tok == Token::RBRACE ||
      tok == Token::EOS) {
    ExpectSemicolon(CHECK_OK);
    return new(zone()) ReturnStatement(GetLiteralUndefined());
  }

  Expression* expr = ParseExpression(true, CHECK_OK);
  ExpectSemicolon(CHECK_OK);
  return new(zone()) ReturnStatement(expr);
}


Statement* Parser::ParseWithStatement(ZoneStringList* labels, bool* ok) {
  // WithStatement ::
  //   'with' '(' Expression ')' Statement

  Expect(Token::WITH, CHECK_OK);

  if (!top_scope_->is_classic_mode()) {
    ReportMessage("strict_mode_with", Vector<const char*>::empty());
    *ok = false;
    return NULL;
  }

  Expect(Token::LPAREN, CHECK_OK);
  Expression* expr = ParseExpression(true, CHECK_OK);
  Expect(Token::RPAREN, CHECK_OK);

  top_scope_->DeclarationScope()->RecordWithStatement();
  Scope* with_scope = NewScope(top_scope_, WITH_SCOPE);
  Statement* stmt;
  { BlockState block_state(this, with_scope);
    with_scope->set_start_position(scanner().peek_location().beg_pos);
    stmt = ParseStatement(labels, CHECK_OK);
    with_scope->set_end_position(scanner().location().end_pos);
  }
  return new(zone()) WithStatement(expr, stmt);
}


CaseClause* Parser::ParseCaseClause(bool* default_seen_ptr, bool* ok) {
  // CaseClause ::
  //   'case' Expression ':' Statement*
  //   'default' ':' Statement*

  Expression* label = NULL;  // NULL expression indicates default case
  if (peek() == Token::CASE) {
    Expect(Token::CASE, CHECK_OK);
    label = ParseExpression(true, CHECK_OK);
  } else {
    Expect(Token::DEFAULT, CHECK_OK);
    if (*default_seen_ptr) {
      ReportMessage("multiple_defaults_in_switch",
                    Vector<const char*>::empty());
      *ok = false;
      return NULL;
    }
    *default_seen_ptr = true;
  }
  Expect(Token::COLON, CHECK_OK);
  int pos = scanner().location().beg_pos;
  ZoneList<Statement*>* statements = new(zone()) ZoneList<Statement*>(5);
  while (peek() != Token::CASE &&
         peek() != Token::DEFAULT &&
         peek() != Token::RBRACE) {
    Statement* stat = ParseStatement(NULL, CHECK_OK);
    statements->Add(stat);
  }

  return new(zone()) CaseClause(isolate(), label, statements, pos);
}


SwitchStatement* Parser::ParseSwitchStatement(ZoneStringList* labels,
                                              bool* ok) {
  // SwitchStatement ::
  //   'switch' '(' Expression ')' '{' CaseClause* '}'

  SwitchStatement* statement = new(zone()) SwitchStatement(isolate(), labels);
  Target target(&this->target_stack_, statement);

  Expect(Token::SWITCH, CHECK_OK);
  Expect(Token::LPAREN, CHECK_OK);
  Expression* tag = ParseExpression(true, CHECK_OK);
  Expect(Token::RPAREN, CHECK_OK);

  bool default_seen = false;
  ZoneList<CaseClause*>* cases = new(zone()) ZoneList<CaseClause*>(4);
  Expect(Token::LBRACE, CHECK_OK);
  while (peek() != Token::RBRACE) {
    CaseClause* clause = ParseCaseClause(&default_seen, CHECK_OK);
    cases->Add(clause);
  }
  Expect(Token::RBRACE, CHECK_OK);

  if (statement) statement->Initialize(tag, cases);
  return statement;
}


Statement* Parser::ParseThrowStatement(bool* ok) {
  // ThrowStatement ::
  //   'throw' Expression ';'

  Expect(Token::THROW, CHECK_OK);
  int pos = scanner().location().beg_pos;
  if (scanner().HasAnyLineTerminatorBeforeNext()) {
    ReportMessage("newline_after_throw", Vector<const char*>::empty());
    *ok = false;
    return NULL;
  }
  Expression* exception = ParseExpression(true, CHECK_OK);
  ExpectSemicolon(CHECK_OK);

  return new(zone()) ExpressionStatement(
      new(zone()) Throw(isolate(), exception, pos));
}


TryStatement* Parser::ParseTryStatement(bool* ok) {
  // TryStatement ::
  //   'try' Block Catch
  //   'try' Block Finally
  //   'try' Block Catch Finally
  //
  // Catch ::
  //   'catch' '(' Identifier ')' Block
  //
  // Finally ::
  //   'finally' Block

  Expect(Token::TRY, CHECK_OK);

  TargetCollector try_collector;
  Block* try_block;

  { Target target(&this->target_stack_, &try_collector);
    try_block = ParseBlock(NULL, CHECK_OK);
  }

  Token::Value tok = peek();
  if (tok != Token::CATCH && tok != Token::FINALLY) {
    ReportMessage("no_catch_or_finally", Vector<const char*>::empty());
    *ok = false;
    return NULL;
  }

  // If we can break out from the catch block and there is a finally block,
  // then we will need to collect escaping targets from the catch
  // block. Since we don't know yet if there will be a finally block, we
  // always collect the targets.
  TargetCollector catch_collector;
  Scope* catch_scope = NULL;
  Variable* catch_variable = NULL;
  Block* catch_block = NULL;
  Handle<String> name;
  if (tok == Token::CATCH) {
    Consume(Token::CATCH);

    Expect(Token::LPAREN, CHECK_OK);
    catch_scope = NewScope(top_scope_, CATCH_SCOPE);
    catch_scope->set_start_position(scanner().location().beg_pos);
    name = ParseIdentifier(CHECK_OK);

    if (!top_scope_->is_classic_mode() && IsEvalOrArguments(name)) {
      ReportMessage("strict_catch_variable", Vector<const char*>::empty());
      *ok = false;
      return NULL;
    }

    Expect(Token::RPAREN, CHECK_OK);

    if (peek() == Token::LBRACE) {
      Target target(&this->target_stack_, &catch_collector);
      VariableMode mode = is_extended_mode() ? LET : VAR;
      catch_variable =
          catch_scope->DeclareLocal(name, mode, kCreatedInitialized);

      BlockState block_state(this, catch_scope);
      catch_block = ParseBlock(NULL, CHECK_OK);
    } else {
      Expect(Token::LBRACE, CHECK_OK);
    }
    catch_scope->set_end_position(scanner().location().end_pos);
    tok = peek();
  }

  Block* finally_block = NULL;
  if (tok == Token::FINALLY || catch_block == NULL) {
    Consume(Token::FINALLY);
    finally_block = ParseBlock(NULL, CHECK_OK);
  }

  // Simplify the AST nodes by converting:
  //   'try B0 catch B1 finally B2'
  // to:
  //   'try { try B0 catch B1 } finally B2'

  if (catch_block != NULL && finally_block != NULL) {
    // If we have both, create an inner try/catch.
    ASSERT(catch_scope != NULL && catch_variable != NULL);
    int index = current_function_state_->NextHandlerIndex();
    TryCatchStatement* statement = new(zone()) TryCatchStatement(index,
                                                                 try_block,
                                                                 catch_scope,
                                                                 catch_variable,
                                                                 catch_block);
    statement->set_escaping_targets(try_collector.targets());
    try_block = new(zone()) Block(isolate(), NULL, 1, false);
    try_block->AddStatement(statement);
    catch_block = NULL;  // Clear to indicate it's been handled.
  }

  TryStatement* result = NULL;
  if (catch_block != NULL) {
    ASSERT(finally_block == NULL);
    ASSERT(catch_scope != NULL && catch_variable != NULL);
    int index = current_function_state_->NextHandlerIndex();
    result = new(zone()) TryCatchStatement(index,
                                           try_block,
                                           catch_scope,
                                           catch_variable,
                                           catch_block);
  } else {
    ASSERT(finally_block != NULL);
    int index = current_function_state_->NextHandlerIndex();
    result = new(zone()) TryFinallyStatement(index,
                                             try_block,
                                             finally_block);
    // Combine the jump targets of the try block and the possible catch block.
    try_collector.targets()->AddAll(*catch_collector.targets());
  }

  result->set_escaping_targets(try_collector.targets());
  return result;
}


DoWhileStatement* Parser::ParseDoWhileStatement(ZoneStringList* labels,
                                                bool* ok) {
  // DoStatement ::
  //   'do' Statement 'while' '(' Expression ')' ';'

  DoWhileStatement* loop = new(zone()) DoWhileStatement(isolate(), labels);
  Target target(&this->target_stack_, loop);

  Expect(Token::DO, CHECK_OK);
  Statement* body = ParseStatement(NULL, CHECK_OK);
  Expect(Token::WHILE, CHECK_OK);
  Expect(Token::LPAREN, CHECK_OK);

  if (loop != NULL) {
    int position = scanner().location().beg_pos;
    loop->set_condition_position(position);
  }

  Expression* cond = ParseExpression(true, CHECK_OK);
  Expect(Token::RPAREN, CHECK_OK);

  // Allow do-statements to be terminated with and without
  // semi-colons. This allows code such as 'do;while(0)return' to
  // parse, which would not be the case if we had used the
  // ExpectSemicolon() functionality here.
  if (peek() == Token::SEMICOLON) Consume(Token::SEMICOLON);

  if (loop != NULL) loop->Initialize(cond, body);
  return loop;
}


WhileStatement* Parser::ParseWhileStatement(ZoneStringList* labels, bool* ok) {
  // WhileStatement ::
  //   'while' '(' Expression ')' Statement

  WhileStatement* loop = new(zone()) WhileStatement(isolate(), labels);
  Target target(&this->target_stack_, loop);

  Expect(Token::WHILE, CHECK_OK);
  Expect(Token::LPAREN, CHECK_OK);
  Expression* cond = ParseExpression(true, CHECK_OK);
  Expect(Token::RPAREN, CHECK_OK);
  Statement* body = ParseStatement(NULL, CHECK_OK);

  if (loop != NULL) loop->Initialize(cond, body);
  return loop;
}


Statement* Parser::ParseForStatement(ZoneStringList* labels, bool* ok) {
  // ForStatement ::
  //   'for' '(' Expression? ';' Expression? ';' Expression? ')' Statement

  Statement* init = NULL;

  // Create an in-between scope for let-bound iteration variables.
  Scope* saved_scope = top_scope_;
  Scope* for_scope = NewScope(top_scope_, BLOCK_SCOPE);
  top_scope_ = for_scope;

  Expect(Token::FOR, CHECK_OK);
  Expect(Token::LPAREN, CHECK_OK);
  for_scope->set_start_position(scanner().location().beg_pos);
  if (peek() != Token::SEMICOLON) {
    if (peek() == Token::VAR || peek() == Token::CONST) {
      Handle<String> name;
      Block* variable_statement =
          ParseVariableDeclarations(kForStatement, NULL, &name, CHECK_OK);

      if (peek() == Token::IN && !name.is_null()) {
        VariableProxy* each = top_scope_->NewUnresolved(name);
        ForInStatement* loop = new(zone()) ForInStatement(isolate(), labels);
        Target target(&this->target_stack_, loop);

        Expect(Token::IN, CHECK_OK);
        Expression* enumerable = ParseExpression(true, CHECK_OK);
        Expect(Token::RPAREN, CHECK_OK);

        Statement* body = ParseStatement(NULL, CHECK_OK);
        loop->Initialize(each, enumerable, body);
        Block* result = new(zone()) Block(isolate(), NULL, 2, false);
        result->AddStatement(variable_statement);
        result->AddStatement(loop);
        top_scope_ = saved_scope;
        for_scope->set_end_position(scanner().location().end_pos);
        for_scope = for_scope->FinalizeBlockScope();
        ASSERT(for_scope == NULL);
        // Parsed for-in loop w/ variable/const declaration.
        return result;
      } else {
        init = variable_statement;
      }
    } else if (peek() == Token::LET) {
      Handle<String> name;
      VariableDeclarationProperties decl_props = kHasNoInitializers;
      Block* variable_statement =
          ParseVariableDeclarations(kForStatement,
                                    &decl_props,
                                    &name,
                                    CHECK_OK);
      bool accept_IN = !name.is_null() && decl_props != kHasInitializers;
      if (peek() == Token::IN && accept_IN) {
        // Rewrite a for-in statement of the form
        //
        //   for (let x in e) b
        //
        // into
        //
        //   <let x' be a temporary variable>
        //   for (x' in e) {
        //     let x;
        //     x = x';
        //     b;
        //   }

        // TODO(keuchel): Move the temporary variable to the block scope, after
        // implementing stack allocated block scoped variables.
        Variable* temp = top_scope_->DeclarationScope()->NewTemporary(name);
        VariableProxy* temp_proxy = new(zone()) VariableProxy(isolate(), temp);
        VariableProxy* each = top_scope_->NewUnresolved(name);
        ForInStatement* loop = new(zone()) ForInStatement(isolate(), labels);
        Target target(&this->target_stack_, loop);

        Expect(Token::IN, CHECK_OK);
        Expression* enumerable = ParseExpression(true, CHECK_OK);
        Expect(Token::RPAREN, CHECK_OK);

        Statement* body = ParseStatement(NULL, CHECK_OK);
        Block* body_block = new(zone()) Block(isolate(), NULL, 3, false);
        Assignment* assignment = new(zone()) Assignment(isolate(),
                                                        Token::ASSIGN,
                                                        each,
                                                        temp_proxy,
                                                        RelocInfo::kNoPosition);
        Statement* assignment_statement =
            new(zone()) ExpressionStatement(assignment);
        body_block->AddStatement(variable_statement);
        body_block->AddStatement(assignment_statement);
        body_block->AddStatement(body);
        loop->Initialize(temp_proxy, enumerable, body_block);
        top_scope_ = saved_scope;
        for_scope->set_end_position(scanner().location().end_pos);
        for_scope = for_scope->FinalizeBlockScope();
        body_block->set_block_scope(for_scope);
        // Parsed for-in loop w/ let declaration.
        return loop;

      } else {
        init = variable_statement;
      }
    } else {
      Expression* expression = ParseExpression(false, CHECK_OK);
      if (peek() == Token::IN) {
        // Signal a reference error if the expression is an invalid
        // left-hand side expression.  We could report this as a syntax
        // error here but for compatibility with JSC we choose to report
        // the error at runtime.
        if (expression == NULL || !expression->IsValidLeftHandSide()) {
          Handle<String> type =
              isolate()->factory()->invalid_lhs_in_for_in_symbol();
          expression = NewThrowReferenceError(type);
        }
        ForInStatement* loop = new(zone()) ForInStatement(isolate(), labels);
        Target target(&this->target_stack_, loop);

        Expect(Token::IN, CHECK_OK);
        Expression* enumerable = ParseExpression(true, CHECK_OK);
        Expect(Token::RPAREN, CHECK_OK);

        Statement* body = ParseStatement(NULL, CHECK_OK);
        if (loop) loop->Initialize(expression, enumerable, body);
        top_scope_ = saved_scope;
        for_scope->set_end_position(scanner().location().end_pos);
        for_scope = for_scope->FinalizeBlockScope();
        ASSERT(for_scope == NULL);
        // Parsed for-in loop.
        return loop;

      } else {
        init = new(zone()) ExpressionStatement(expression);
      }
    }
  }

  // Standard 'for' loop
  ForStatement* loop = new(zone()) ForStatement(isolate(), labels);
  Target target(&this->target_stack_, loop);

  // Parsed initializer at this point.
  Expect(Token::SEMICOLON, CHECK_OK);

  Expression* cond = NULL;
  if (peek() != Token::SEMICOLON) {
    cond = ParseExpression(true, CHECK_OK);
  }
  Expect(Token::SEMICOLON, CHECK_OK);

  Statement* next = NULL;
  if (peek() != Token::RPAREN) {
    Expression* exp = ParseExpression(true, CHECK_OK);
    next = new(zone()) ExpressionStatement(exp);
  }
  Expect(Token::RPAREN, CHECK_OK);

  Statement* body = ParseStatement(NULL, CHECK_OK);
  top_scope_ = saved_scope;
  for_scope->set_end_position(scanner().location().end_pos);
  for_scope = for_scope->FinalizeBlockScope();
  if (for_scope != NULL) {
    // Rewrite a for statement of the form
    //
    //   for (let x = i; c; n) b
    //
    // into
    //
    //   {
    //     let x = i;
    //     for (; c; n) b
    //   }
    ASSERT(init != NULL);
    Block* result = new(zone()) Block(isolate(), NULL, 2, false);
    result->AddStatement(init);
    result->AddStatement(loop);
    result->set_block_scope(for_scope);
    if (loop) loop->Initialize(NULL, cond, next, body);
    return result;
  } else {
    if (loop) loop->Initialize(init, cond, next, body);
    return loop;
  }
}


// Precedence = 1
Expression* Parser::ParseExpression(bool accept_IN, bool* ok) {
  // Expression ::
  //   AssignmentExpression
  //   Expression ',' AssignmentExpression

  Expression* result = ParseAssignmentExpression(accept_IN, CHECK_OK);
  while (peek() == Token::COMMA) {
    Expect(Token::COMMA, CHECK_OK);
    int position = scanner().location().beg_pos;
    Expression* right = ParseAssignmentExpression(accept_IN, CHECK_OK);
    result = new(zone()) BinaryOperation(
        isolate(), Token::COMMA, result, right, position);
  }
  return result;
}


// Precedence = 2
Expression* Parser::ParseAssignmentExpression(bool accept_IN, bool* ok) {
  // AssignmentExpression ::
  //   ConditionalExpression
  //   LeftHandSideExpression AssignmentOperator AssignmentExpression

  if (fni_ != NULL) fni_->Enter();
  Expression* expression = ParseConditionalExpression(accept_IN, CHECK_OK);

  if (!Token::IsAssignmentOp(peek())) {
    if (fni_ != NULL) fni_->Leave();
    // Parsed conditional expression only (no assignment).
    return expression;
  }

  // Signal a reference error if the expression is an invalid left-hand
  // side expression.  We could report this as a syntax error here but
  // for compatibility with JSC we choose to report the error at
  // runtime.
  if (expression == NULL || !expression->IsValidLeftHandSide()) {
    Handle<String> type =
        isolate()->factory()->invalid_lhs_in_assignment_symbol();
    expression = NewThrowReferenceError(type);
  }

  if (!top_scope_->is_classic_mode()) {
    // Assignment to eval or arguments is disallowed in strict mode.
    CheckStrictModeLValue(expression, "strict_lhs_assignment", CHECK_OK);
  }

  Token::Value op = Next();  // Get assignment operator.
  int pos = scanner().location().beg_pos;
  Expression* right = ParseAssignmentExpression(accept_IN, CHECK_OK);

  // TODO(1231235): We try to estimate the set of properties set by
  // constructors. We define a new property whenever there is an
  // assignment to a property of 'this'. We should probably only add
  // properties if we haven't seen them before. Otherwise we'll
  // probably overestimate the number of properties.
  Property* property = expression ? expression->AsProperty() : NULL;
  if (op == Token::ASSIGN &&
      property != NULL &&
      property->obj()->AsVariableProxy() != NULL &&
      property->obj()->AsVariableProxy()->is_this()) {
    current_function_state_->AddProperty();
  }

  // If we assign a function literal to a property we pretenure the
  // literal so it can be added as a constant function property.
  if (property != NULL && right->AsFunctionLiteral() != NULL) {
    right->AsFunctionLiteral()->set_pretenure();
  }

  if (fni_ != NULL) {
    // Check if the right hand side is a call to avoid inferring a
    // name if we're dealing with "a = function(){...}();"-like
    // expression.
    if ((op == Token::INIT_VAR
         || op == Token::INIT_CONST
         || op == Token::ASSIGN)
        && (right->AsCall() == NULL && right->AsCallNew() == NULL)) {
      fni_->Infer();
    } else {
      fni_->RemoveLastFunction();
    }
    fni_->Leave();
  }

  return new(zone()) Assignment(isolate(), op, expression, right, pos);
}


// Precedence = 3
Expression* Parser::ParseConditionalExpression(bool accept_IN, bool* ok) {
  // ConditionalExpression ::
  //   LogicalOrExpression
  //   LogicalOrExpression '?' AssignmentExpression ':' AssignmentExpression

  // We start using the binary expression parser for prec >= 4 only!
  Expression* expression = ParseBinaryExpression(4, accept_IN, CHECK_OK);
  if (peek() != Token::CONDITIONAL) return expression;
  Consume(Token::CONDITIONAL);
  // In parsing the first assignment expression in conditional
  // expressions we always accept the 'in' keyword; see ECMA-262,
  // section 11.12, page 58.
  int left_position = scanner().peek_location().beg_pos;
  Expression* left = ParseAssignmentExpression(true, CHECK_OK);
  Expect(Token::COLON, CHECK_OK);
  int right_position = scanner().peek_location().beg_pos;
  Expression* right = ParseAssignmentExpression(accept_IN, CHECK_OK);
  return new(zone()) Conditional(
      isolate(), expression, left, right, left_position, right_position);
}


static int Precedence(Token::Value tok, bool accept_IN) {
  if (tok == Token::IN && !accept_IN)
    return 0;  // 0 precedence will terminate binary expression parsing

  return Token::Precedence(tok);
}


// Precedence >= 4
Expression* Parser::ParseBinaryExpression(int prec, bool accept_IN, bool* ok) {
  ASSERT(prec >= 4);
  Expression* x = ParseUnaryExpression(CHECK_OK);
  for (int prec1 = Precedence(peek(), accept_IN); prec1 >= prec; prec1--) {
    // prec1 >= 4
    while (Precedence(peek(), accept_IN) == prec1) {
      Token::Value op = Next();
      int position = scanner().location().beg_pos;
      Expression* y = ParseBinaryExpression(prec1 + 1, accept_IN, CHECK_OK);

      // Compute some expressions involving only number literals.
      if (x && x->AsLiteral() && x->AsLiteral()->handle()->IsNumber() &&
          y && y->AsLiteral() && y->AsLiteral()->handle()->IsNumber()) {
        double x_val = x->AsLiteral()->handle()->Number();
        double y_val = y->AsLiteral()->handle()->Number();

        switch (op) {
          case Token::ADD:
            x = NewNumberLiteral(x_val + y_val);
            continue;
          case Token::SUB:
            x = NewNumberLiteral(x_val - y_val);
            continue;
          case Token::MUL:
            x = NewNumberLiteral(x_val * y_val);
            continue;
          case Token::DIV:
            x = NewNumberLiteral(x_val / y_val);
            continue;
          case Token::BIT_OR:
            x = NewNumberLiteral(DoubleToInt32(x_val) | DoubleToInt32(y_val));
            continue;
          case Token::BIT_AND:
            x = NewNumberLiteral(DoubleToInt32(x_val) & DoubleToInt32(y_val));
            continue;
          case Token::BIT_XOR:
            x = NewNumberLiteral(DoubleToInt32(x_val) ^ DoubleToInt32(y_val));
            continue;
          case Token::SHL: {
            int value = DoubleToInt32(x_val) << (DoubleToInt32(y_val) & 0x1f);
            x = NewNumberLiteral(value);
            continue;
          }
          case Token::SHR: {
            uint32_t shift = DoubleToInt32(y_val) & 0x1f;
            uint32_t value = DoubleToUint32(x_val) >> shift;
            x = NewNumberLiteral(value);
            continue;
          }
          case Token::SAR: {
            uint32_t shift = DoubleToInt32(y_val) & 0x1f;
            int value = ArithmeticShiftRight(DoubleToInt32(x_val), shift);
            x = NewNumberLiteral(value);
            continue;
          }
          default:
            break;
        }
      }

      // For now we distinguish between comparisons and other binary
      // operations.  (We could combine the two and get rid of this
      // code and AST node eventually.)
      if (Token::IsCompareOp(op)) {
        // We have a comparison.
        Token::Value cmp = op;
        switch (op) {
          case Token::NE: cmp = Token::EQ; break;
          case Token::NE_STRICT: cmp = Token::EQ_STRICT; break;
          default: break;
        }
        x = new(zone()) CompareOperation(isolate(), cmp, x, y, position);
        if (cmp != op) {
          // The comparison was negated - add a NOT.
          x = new(zone()) UnaryOperation(isolate(), Token::NOT, x, position);
        }

      } else {
        // We have a "normal" binary operation.
        x = new(zone()) BinaryOperation(isolate(), op, x, y, position);
      }
    }
  }
  return x;
}


Expression* Parser::ParseUnaryExpression(bool* ok) {
  // UnaryExpression ::
  //   PostfixExpression
  //   'delete' UnaryExpression
  //   'void' UnaryExpression
  //   'typeof' UnaryExpression
  //   '++' UnaryExpression
  //   '--' UnaryExpression
  //   '+' UnaryExpression
  //   '-' UnaryExpression
  //   '~' UnaryExpression
  //   '!' UnaryExpression

  Token::Value op = peek();
  if (Token::IsUnaryOp(op)) {
    op = Next();
    int position = scanner().location().beg_pos;
    Expression* expression = ParseUnaryExpression(CHECK_OK);

    if (expression != NULL && (expression->AsLiteral() != NULL)) {
      Handle<Object> literal = expression->AsLiteral()->handle();
      if (op == Token::NOT) {
        // Convert the literal to a boolean condition and negate it.
        bool condition = literal->ToBoolean()->IsTrue();
        Handle<Object> result(isolate()->heap()->ToBoolean(!condition));
        return NewLiteral(result);
      } else if (literal->IsNumber()) {
        // Compute some expressions involving only number literals.
        double value = literal->Number();
        switch (op) {
          case Token::ADD:
            return expression;
          case Token::SUB:
            return NewNumberLiteral(-value);
          case Token::BIT_NOT:
            return NewNumberLiteral(~DoubleToInt32(value));
          default:
            break;
        }
      }
    }

    // "delete identifier" is a syntax error in strict mode.
    if (op == Token::DELETE && !top_scope_->is_classic_mode()) {
      VariableProxy* operand = expression->AsVariableProxy();
      if (operand != NULL && !operand->is_this()) {
        ReportMessage("strict_delete", Vector<const char*>::empty());
        *ok = false;
        return NULL;
      }
    }

    return new(zone()) UnaryOperation(isolate(), op, expression, position);

  } else if (Token::IsCountOp(op)) {
    op = Next();
    Expression* expression = ParseUnaryExpression(CHECK_OK);
    // Signal a reference error if the expression is an invalid
    // left-hand side expression.  We could report this as a syntax
    // error here but for compatibility with JSC we choose to report the
    // error at runtime.
    if (expression == NULL || !expression->IsValidLeftHandSide()) {
      Handle<String> type =
          isolate()->factory()->invalid_lhs_in_prefix_op_symbol();
      expression = NewThrowReferenceError(type);
    }

    if (!top_scope_->is_classic_mode()) {
      // Prefix expression operand in strict mode may not be eval or arguments.
      CheckStrictModeLValue(expression, "strict_lhs_prefix", CHECK_OK);
    }

    int position = scanner().location().beg_pos;
    return new(zone()) CountOperation(isolate(),
                                      op,
                                      true /* prefix */,
                                      expression,
                                      position);

  } else {
    return ParsePostfixExpression(ok);
  }
}


Expression* Parser::ParsePostfixExpression(bool* ok) {
  // PostfixExpression ::
  //   LeftHandSideExpression ('++' | '--')?

  Expression* expression = ParseLeftHandSideExpression(CHECK_OK);
  if (!scanner().HasAnyLineTerminatorBeforeNext() &&
      Token::IsCountOp(peek())) {
    // Signal a reference error if the expression is an invalid
    // left-hand side expression.  We could report this as a syntax
    // error here but for compatibility with JSC we choose to report the
    // error at runtime.
    if (expression == NULL || !expression->IsValidLeftHandSide()) {
      Handle<String> type =
          isolate()->factory()->invalid_lhs_in_postfix_op_symbol();
      expression = NewThrowReferenceError(type);
    }

    if (!top_scope_->is_classic_mode()) {
      // Postfix expression operand in strict mode may not be eval or arguments.
      CheckStrictModeLValue(expression, "strict_lhs_prefix", CHECK_OK);
    }

    Token::Value next = Next();
    int position = scanner().location().beg_pos;
    expression =
        new(zone()) CountOperation(isolate(),
                                   next,
                                   false /* postfix */,
                                   expression,
                                   position);
  }
  return expression;
}


Expression* Parser::ParseLeftHandSideExpression(bool* ok) {
  // LeftHandSideExpression ::
  //   (NewExpression | MemberExpression) ...

  Expression* result;
  if (peek() == Token::NEW) {
    result = ParseNewExpression(CHECK_OK);
  } else {
    result = ParseMemberExpression(CHECK_OK);
  }

  while (true) {
    switch (peek()) {
      case Token::LBRACK: {
        Consume(Token::LBRACK);
        int pos = scanner().location().beg_pos;
        Expression* index = ParseExpression(true, CHECK_OK);
        result = new(zone()) Property(isolate(), result, index, pos);
        Expect(Token::RBRACK, CHECK_OK);
        break;
      }

      case Token::LPAREN: {
        int pos = scanner().location().beg_pos;
        ZoneList<Expression*>* args = ParseArguments(CHECK_OK);

        // Keep track of eval() calls since they disable all local variable
        // optimizations.
        // The calls that need special treatment are the
        // direct eval calls. These calls are all of the form eval(...), with
        // no explicit receiver.
        // These calls are marked as potentially direct eval calls. Whether
        // they are actually direct calls to eval is determined at run time.
        VariableProxy* callee = result->AsVariableProxy();
        if (callee != NULL &&
            callee->IsVariable(isolate()->factory()->eval_symbol())) {
          top_scope_->DeclarationScope()->RecordEvalCall();
        }
        result = NewCall(result, args, pos);
        break;
      }

      case Token::PERIOD: {
        Consume(Token::PERIOD);
        int pos = scanner().location().beg_pos;
        Handle<String> name = ParseIdentifierName(CHECK_OK);
        result = new(zone()) Property(isolate(),
                                      result,
                                      NewLiteral(name),
                                      pos);
        if (fni_ != NULL) fni_->PushLiteralName(name);
        break;
      }

      default:
        return result;
    }
  }
}


Expression* Parser::ParseNewPrefix(PositionStack* stack, bool* ok) {
  // NewExpression ::
  //   ('new')+ MemberExpression

  // The grammar for new expressions is pretty warped. The keyword
  // 'new' can either be a part of the new expression (where it isn't
  // followed by an argument list) or a part of the member expression,
  // where it must be followed by an argument list. To accommodate
  // this, we parse the 'new' keywords greedily and keep track of how
  // many we have parsed. This information is then passed on to the
  // member expression parser, which is only allowed to match argument
  // lists as long as it has 'new' prefixes left
  Expect(Token::NEW, CHECK_OK);
  PositionStack::Element pos(stack, scanner().location().beg_pos);

  Expression* result;
  if (peek() == Token::NEW) {
    result = ParseNewPrefix(stack, CHECK_OK);
  } else {
    result = ParseMemberWithNewPrefixesExpression(stack, CHECK_OK);
  }

  if (!stack->is_empty()) {
    int last = stack->pop();
    result = new(zone()) CallNew(isolate(),
                                 result,
                                 new(zone()) ZoneList<Expression*>(0),
                                 last);
  }
  return result;
}


Expression* Parser::ParseNewExpression(bool* ok) {
  PositionStack stack(ok);
  return ParseNewPrefix(&stack, ok);
}


Expression* Parser::ParseMemberExpression(bool* ok) {
  return ParseMemberWithNewPrefixesExpression(NULL, ok);
}


Expression* Parser::ParseMemberWithNewPrefixesExpression(PositionStack* stack,
                                                         bool* ok) {
  // MemberExpression ::
  //   (PrimaryExpression | FunctionLiteral)
  //     ('[' Expression ']' | '.' Identifier | Arguments)*

  // Parse the initial primary or function expression.
  Expression* result = NULL;
  if (peek() == Token::FUNCTION) {
    Expect(Token::FUNCTION, CHECK_OK);
    int function_token_position = scanner().location().beg_pos;
    Handle<String> name;
    bool is_strict_reserved_name = false;
    if (peek_any_identifier()) {
      name = ParseIdentifierOrStrictReservedWord(&is_strict_reserved_name,
                                                 CHECK_OK);
    }
    FunctionLiteral::Type type = name.is_null()
        ? FunctionLiteral::ANONYMOUS_EXPRESSION
        : FunctionLiteral::NAMED_EXPRESSION;
    result = ParseFunctionLiteral(name,
                                  is_strict_reserved_name,
                                  function_token_position,
                                  type,
                                  CHECK_OK);
  } else {
    result = ParsePrimaryExpression(CHECK_OK);
  }

  while (true) {
    switch (peek()) {
      case Token::LBRACK: {
        Consume(Token::LBRACK);
        int pos = scanner().location().beg_pos;
        Expression* index = ParseExpression(true, CHECK_OK);
        result = new(zone()) Property(isolate(), result, index, pos);
        if (fni_ != NULL) {
          if (index->IsPropertyName()) {
            fni_->PushLiteralName(index->AsLiteral()->AsPropertyName());
          } else {
            fni_->PushLiteralName(
                isolate()->factory()->anonymous_function_symbol());
          }
        }
        Expect(Token::RBRACK, CHECK_OK);
        break;
      }
      case Token::PERIOD: {
        Consume(Token::PERIOD);
        int pos = scanner().location().beg_pos;
        Handle<String> name = ParseIdentifierName(CHECK_OK);
        result = new(zone()) Property(isolate(),
                                      result,
                                      NewLiteral(name),
                                      pos);
        if (fni_ != NULL) fni_->PushLiteralName(name);
        break;
      }
      case Token::LPAREN: {
        if ((stack == NULL) || stack->is_empty()) return result;
        // Consume one of the new prefixes (already parsed).
        ZoneList<Expression*>* args = ParseArguments(CHECK_OK);
        int last = stack->pop();
        result = new(zone()) CallNew(isolate(), result, args, last);
        break;
      }
      default:
        return result;
    }
  }
}


DebuggerStatement* Parser::ParseDebuggerStatement(bool* ok) {
  // In ECMA-262 'debugger' is defined as a reserved keyword. In some browser
  // contexts this is used as a statement which invokes the debugger as i a
  // break point is present.
  // DebuggerStatement ::
  //   'debugger' ';'

  Expect(Token::DEBUGGER, CHECK_OK);
  ExpectSemicolon(CHECK_OK);
  return new(zone()) DebuggerStatement();
}


void Parser::ReportUnexpectedToken(Token::Value token) {
  // We don't report stack overflows here, to avoid increasing the
  // stack depth even further.  Instead we report it after parsing is
  // over, in ParseProgram/ParseJson.
  if (token == Token::ILLEGAL && stack_overflow_) return;
  // Four of the tokens are treated specially
  switch (token) {
    case Token::EOS:
      return ReportMessage("unexpected_eos", Vector<const char*>::empty());
    case Token::NUMBER:
      return ReportMessage("unexpected_token_number",
                           Vector<const char*>::empty());
    case Token::STRING:
      return ReportMessage("unexpected_token_string",
                           Vector<const char*>::empty());
    case Token::IDENTIFIER:
      return ReportMessage("unexpected_token_identifier",
                           Vector<const char*>::empty());
    case Token::FUTURE_RESERVED_WORD:
      return ReportMessage("unexpected_reserved",
                           Vector<const char*>::empty());
    case Token::FUTURE_STRICT_RESERVED_WORD:
      return ReportMessage(top_scope_->is_classic_mode() ?
                               "unexpected_token_identifier" :
                               "unexpected_strict_reserved",
                           Vector<const char*>::empty());
    default:
      const char* name = Token::String(token);
      ASSERT(name != NULL);
      ReportMessage("unexpected_token", Vector<const char*>(&name, 1));
  }
}


void Parser::ReportInvalidPreparseData(Handle<String> name, bool* ok) {
  SmartArrayPointer<char> name_string = name->ToCString(DISALLOW_NULLS);
  const char* element[1] = { *name_string };
  ReportMessage("invalid_preparser_data",
                Vector<const char*>(element, 1));
  *ok = false;
}


Expression* Parser::ParsePrimaryExpression(bool* ok) {
  // PrimaryExpression ::
  //   'this'
  //   'null'
  //   'true'
  //   'false'
  //   Identifier
  //   Number
  //   String
  //   ArrayLiteral
  //   ObjectLiteral
  //   RegExpLiteral
  //   '(' Expression ')'

  Expression* result = NULL;
  switch (peek()) {
    case Token::THIS: {
      Consume(Token::THIS);
      result = new(zone()) VariableProxy(isolate(), top_scope_->receiver());
      break;
    }

    case Token::NULL_LITERAL:
      Consume(Token::NULL_LITERAL);
      result = new(zone()) Literal(
          isolate(), isolate()->factory()->null_value());
      break;

    case Token::TRUE_LITERAL:
      Consume(Token::TRUE_LITERAL);
      result = new(zone()) Literal(
          isolate(), isolate()->factory()->true_value());
      break;

    case Token::FALSE_LITERAL:
      Consume(Token::FALSE_LITERAL);
      result = new(zone()) Literal(
          isolate(), isolate()->factory()->false_value());
      break;

    case Token::IDENTIFIER:
    case Token::FUTURE_STRICT_RESERVED_WORD: {
      Handle<String> name = ParseIdentifier(CHECK_OK);
      if (fni_ != NULL) fni_->PushVariableName(name);
      result = top_scope_->NewUnresolved(name, scanner().location().beg_pos);
      break;
    }

    case Token::NUMBER: {
      Consume(Token::NUMBER);
      ASSERT(scanner().is_literal_ascii());
      double value = StringToDouble(isolate()->unicode_cache(),
                                    scanner().literal_ascii_string(),
                                    ALLOW_HEX | ALLOW_OCTALS);
      result = NewNumberLiteral(value);
      break;
    }

    case Token::STRING: {
      Consume(Token::STRING);
      Handle<String> symbol = GetSymbol(CHECK_OK);
      result = NewLiteral(symbol);
      if (fni_ != NULL) fni_->PushLiteralName(symbol);
      break;
    }

    case Token::ASSIGN_DIV:
      result = ParseRegExpLiteral(true, CHECK_OK);
      break;

    case Token::DIV:
      result = ParseRegExpLiteral(false, CHECK_OK);
      break;

    case Token::LBRACK:
      result = ParseArrayLiteral(CHECK_OK);
      break;

    case Token::LBRACE:
      result = ParseObjectLiteral(CHECK_OK);
      break;

    case Token::LPAREN:
      Consume(Token::LPAREN);
      // Heuristically try to detect immediately called functions before
      // seeing the call parentheses.
      parenthesized_function_ = (peek() == Token::FUNCTION);
      result = ParseExpression(true, CHECK_OK);
      Expect(Token::RPAREN, CHECK_OK);
      break;

    case Token::MOD:
      if (allow_natives_syntax_ || extension_ != NULL) {
        result = ParseV8Intrinsic(CHECK_OK);
        break;
      }
      // If we're not allowing special syntax we fall-through to the
      // default case.

    default: {
      Token::Value tok = Next();
      ReportUnexpectedToken(tok);
      *ok = false;
      return NULL;
    }
  }

  return result;
}


void Parser::BuildArrayLiteralBoilerplateLiterals(ZoneList<Expression*>* values,
                                                  Handle<FixedArray> literals,
                                                  bool* is_simple,
                                                  int* depth) {
  // Fill in the literals.
  // Accumulate output values in local variables.
  bool is_simple_acc = true;
  int depth_acc = 1;
  for (int i = 0; i < values->length(); i++) {
    MaterializedLiteral* m_literal = values->at(i)->AsMaterializedLiteral();
    if (m_literal != NULL && m_literal->depth() >= depth_acc) {
      depth_acc = m_literal->depth() + 1;
    }
    Handle<Object> boilerplate_value = GetBoilerplateValue(values->at(i));
    if (boilerplate_value->IsUndefined()) {
      literals->set_the_hole(i);
      is_simple_acc = false;
    } else {
      literals->set(i, *boilerplate_value);
    }
  }

  *is_simple = is_simple_acc;
  *depth = depth_acc;
}


Expression* Parser::ParseArrayLiteral(bool* ok) {
  // ArrayLiteral ::
  //   '[' Expression? (',' Expression?)* ']'

  ZoneList<Expression*>* values = new(zone()) ZoneList<Expression*>(4);
  Expect(Token::LBRACK, CHECK_OK);
  while (peek() != Token::RBRACK) {
    Expression* elem;
    if (peek() == Token::COMMA) {
      elem = GetLiteralTheHole();
    } else {
      elem = ParseAssignmentExpression(true, CHECK_OK);
    }
    values->Add(elem);
    if (peek() != Token::RBRACK) {
      Expect(Token::COMMA, CHECK_OK);
    }
  }
  Expect(Token::RBRACK, CHECK_OK);

  // Update the scope information before the pre-parsing bailout.
  int literal_index = current_function_state_->NextMaterializedLiteralIndex();

  // Allocate a fixed array to hold all the object literals.
  Handle<FixedArray> object_literals =
      isolate()->factory()->NewFixedArray(values->length(), TENURED);
  Handle<FixedDoubleArray> double_literals;
  ElementsKind elements_kind = FAST_SMI_ONLY_ELEMENTS;

  // Fill in the literals.
  bool is_simple = true;
  int depth = 1;
  for (int i = 0, n = values->length(); i < n; i++) {
    MaterializedLiteral* m_literal = values->at(i)->AsMaterializedLiteral();
    if (m_literal != NULL && m_literal->depth() + 1 > depth) {
      depth = m_literal->depth() + 1;
    }
    Handle<Object> boilerplate_value = GetBoilerplateValue(values->at(i));
    if (boilerplate_value->IsUndefined()) {
      object_literals->set_the_hole(i);
      if (elements_kind == FAST_DOUBLE_ELEMENTS) {
        double_literals->set_the_hole(i);
      }
      is_simple = false;
    } else {
      // Examine each literal element, and adjust the ElementsKind if the
      // literal element is not of a type that can be stored in the current
      // ElementsKind.  Start with FAST_SMI_ONLY_ELEMENTS, and transition to
      // FAST_DOUBLE_ELEMENTS and FAST_ELEMENTS as necessary.  Always remember
      // the tagged value, no matter what the ElementsKind is in case we
      // ultimately end up in FAST_ELEMENTS.
      object_literals->set(i, *boilerplate_value);
      if (elements_kind == FAST_SMI_ONLY_ELEMENTS) {
        // Smi only elements. Notice if a transition to FAST_DOUBLE_ELEMENTS or
        // FAST_ELEMENTS is required.
        if (!boilerplate_value->IsSmi()) {
          if (boilerplate_value->IsNumber() && FLAG_smi_only_arrays) {
            // Allocate a double array on the FAST_DOUBLE_ELEMENTS transition to
            // avoid over-allocating in TENURED space.
            double_literals = isolate()->factory()->NewFixedDoubleArray(
                values->length(), TENURED);
            // Copy the contents of the FAST_SMI_ONLY_ELEMENT array to the
            // FAST_DOUBLE_ELEMENTS array so that they are in sync.
            for (int j = 0; j < i; ++j) {
              Object* smi_value = object_literals->get(j);
              if (smi_value->IsTheHole()) {
                double_literals->set_the_hole(j);
              } else {
                double_literals->set(j, Smi::cast(smi_value)->value());
              }
            }
            double_literals->set(i, boilerplate_value->Number());
            elements_kind = FAST_DOUBLE_ELEMENTS;
          } else {
            elements_kind = FAST_ELEMENTS;
          }
        }
      } else if (elements_kind == FAST_DOUBLE_ELEMENTS) {
        // Continue to store double values in to FAST_DOUBLE_ELEMENTS arrays
        // until the first value is seen that can't be stored as a double.
        if (boilerplate_value->IsNumber()) {
          double_literals->set(i, boilerplate_value->Number());
        } else {
          elements_kind = FAST_ELEMENTS;
        }
      }
    }
  }

  // Simple and shallow arrays can be lazily copied, we transform the
  // elements array to a copy-on-write array.
  if (is_simple && depth == 1 && values->length() > 0 &&
      elements_kind != FAST_DOUBLE_ELEMENTS) {
    object_literals->set_map(isolate()->heap()->fixed_cow_array_map());
  }

  Handle<FixedArrayBase> element_values = elements_kind == FAST_DOUBLE_ELEMENTS
      ? Handle<FixedArrayBase>(double_literals)
      : Handle<FixedArrayBase>(object_literals);

  // Remember both the literal's constant values as well as the ElementsKind
  // in a 2-element FixedArray.
  Handle<FixedArray> literals =
      isolate()->factory()->NewFixedArray(2, TENURED);

  literals->set(0, Smi::FromInt(elements_kind));
  literals->set(1, *element_values);

  return new(zone()) ArrayLiteral(
      isolate(), literals, values, literal_index, is_simple, depth);
}


bool Parser::IsBoilerplateProperty(ObjectLiteral::Property* property) {
  return property != NULL &&
         property->kind() != ObjectLiteral::Property::PROTOTYPE;
}


bool CompileTimeValue::IsCompileTimeValue(Expression* expression) {
  if (expression->AsLiteral() != NULL) return true;
  MaterializedLiteral* lit = expression->AsMaterializedLiteral();
  return lit != NULL && lit->is_simple();
}


bool CompileTimeValue::ArrayLiteralElementNeedsInitialization(
    Expression* value) {
  // If value is a literal the property value is already set in the
  // boilerplate object.
  if (value->AsLiteral() != NULL) return false;
  // If value is a materialized literal the property value is already set
  // in the boilerplate object if it is simple.
  if (CompileTimeValue::IsCompileTimeValue(value)) return false;
  return true;
}


Handle<FixedArray> CompileTimeValue::GetValue(Expression* expression) {
  ASSERT(IsCompileTimeValue(expression));
  Handle<FixedArray> result = FACTORY->NewFixedArray(2, TENURED);
  ObjectLiteral* object_literal = expression->AsObjectLiteral();
  if (object_literal != NULL) {
    ASSERT(object_literal->is_simple());
    if (object_literal->fast_elements()) {
      result->set(kTypeSlot, Smi::FromInt(OBJECT_LITERAL_FAST_ELEMENTS));
    } else {
      result->set(kTypeSlot, Smi::FromInt(OBJECT_LITERAL_SLOW_ELEMENTS));
    }
    result->set(kElementsSlot, *object_literal->constant_properties());
  } else {
    ArrayLiteral* array_literal = expression->AsArrayLiteral();
    ASSERT(array_literal != NULL && array_literal->is_simple());
    result->set(kTypeSlot, Smi::FromInt(ARRAY_LITERAL));
    result->set(kElementsSlot, *array_literal->constant_elements());
  }
  return result;
}


CompileTimeValue::Type CompileTimeValue::GetType(Handle<FixedArray> value) {
  Smi* type_value = Smi::cast(value->get(kTypeSlot));
  return static_cast<Type>(type_value->value());
}


Handle<FixedArray> CompileTimeValue::GetElements(Handle<FixedArray> value) {
  return Handle<FixedArray>(FixedArray::cast(value->get(kElementsSlot)));
}


Handle<Object> Parser::GetBoilerplateValue(Expression* expression) {
  if (expression->AsLiteral() != NULL) {
    return expression->AsLiteral()->handle();
  }
  if (CompileTimeValue::IsCompileTimeValue(expression)) {
    return CompileTimeValue::GetValue(expression);
  }
  return isolate()->factory()->undefined_value();
}

// Defined in ast.cc
bool IsEqualString(void* first, void* second);
bool IsEqualNumber(void* first, void* second);


// Validation per 11.1.5 Object Initialiser
class ObjectLiteralPropertyChecker {
 public:
  ObjectLiteralPropertyChecker(Parser* parser, LanguageMode language_mode) :
    props(&IsEqualString),
    elems(&IsEqualNumber),
    parser_(parser),
    language_mode_(language_mode) {
  }

  void CheckProperty(
    ObjectLiteral::Property* property,
    Scanner::Location loc,
    bool* ok);

 private:
  enum PropertyKind {
    kGetAccessor = 0x01,
    kSetAccessor = 0x02,
    kAccessor = kGetAccessor | kSetAccessor,
    kData = 0x04
  };

  static intptr_t GetPropertyKind(ObjectLiteral::Property* property) {
    switch (property->kind()) {
      case ObjectLiteral::Property::GETTER:
        return kGetAccessor;
      case ObjectLiteral::Property::SETTER:
        return kSetAccessor;
      default:
        return kData;
    }
  }

  HashMap props;
  HashMap elems;
  Parser* parser_;
  LanguageMode language_mode_;
};


void ObjectLiteralPropertyChecker::CheckProperty(
    ObjectLiteral::Property* property,
    Scanner::Location loc,
    bool* ok) {

  ASSERT(property != NULL);

  Literal *lit = property->key();
  Handle<Object> handle = lit->handle();

  uint32_t hash;
  HashMap* map;
  void* key;

  if (handle->IsSymbol()) {
    Handle<String> name(String::cast(*handle));
    if (name->AsArrayIndex(&hash)) {
      Handle<Object> key_handle = FACTORY->NewNumberFromUint(hash);
      key = key_handle.location();
      map = &elems;
    } else {
      key = handle.location();
      hash = name->Hash();
      map = &props;
    }
  } else if (handle->ToArrayIndex(&hash)) {
    key = handle.location();
    map = &elems;
  } else {
    ASSERT(handle->IsNumber());
    double num = handle->Number();
    char arr[100];
    Vector<char> buffer(arr, ARRAY_SIZE(arr));
    const char* str = DoubleToCString(num, buffer);
    Handle<String> name = FACTORY->NewStringFromAscii(CStrVector(str));
    key = name.location();
    hash = name->Hash();
    map = &props;
  }

  // Lookup property previously defined, if any.
  HashMap::Entry* entry = map->Lookup(key, hash, true);
  intptr_t prev = reinterpret_cast<intptr_t> (entry->value);
  intptr_t curr = GetPropertyKind(property);

  // Duplicate data properties are illegal in strict or extended mode.
  if (language_mode_ != CLASSIC_MODE && (curr & prev & kData) != 0) {
    parser_->ReportMessageAt(loc, "strict_duplicate_property",
                             Vector<const char*>::empty());
    *ok = false;
    return;
  }
  // Data property conflicting with an accessor.
  if (((curr & kData) && (prev & kAccessor)) ||
      ((prev & kData) && (curr & kAccessor))) {
    parser_->ReportMessageAt(loc, "accessor_data_property",
                             Vector<const char*>::empty());
    *ok = false;
    return;
  }
  // Two accessors of the same type conflicting
  if ((curr & prev & kAccessor) != 0) {
    parser_->ReportMessageAt(loc, "accessor_get_set",
                             Vector<const char*>::empty());
    *ok = false;
    return;
  }

  // Update map
  entry->value = reinterpret_cast<void*> (prev | curr);
  *ok = true;
}


void Parser::BuildObjectLiteralConstantProperties(
    ZoneList<ObjectLiteral::Property*>* properties,
    Handle<FixedArray> constant_properties,
    bool* is_simple,
    bool* fast_elements,
    int* depth) {
  int position = 0;
  // Accumulate the value in local variables and store it at the end.
  bool is_simple_acc = true;
  int depth_acc = 1;
  uint32_t max_element_index = 0;
  uint32_t elements = 0;
  for (int i = 0; i < properties->length(); i++) {
    ObjectLiteral::Property* property = properties->at(i);
    if (!IsBoilerplateProperty(property)) {
      is_simple_acc = false;
      continue;
    }
    MaterializedLiteral* m_literal = property->value()->AsMaterializedLiteral();
    if (m_literal != NULL && m_literal->depth() >= depth_acc) {
      depth_acc = m_literal->depth() + 1;
    }

    // Add CONSTANT and COMPUTED properties to boilerplate. Use undefined
    // value for COMPUTED properties, the real value is filled in at
    // runtime. The enumeration order is maintained.
    Handle<Object> key = property->key()->handle();
    Handle<Object> value = GetBoilerplateValue(property->value());
    is_simple_acc = is_simple_acc && !value->IsUndefined();

    // Keep track of the number of elements in the object literal and
    // the largest element index.  If the largest element index is
    // much larger than the number of elements, creating an object
    // literal with fast elements will be a waste of space.
    uint32_t element_index = 0;
    if (key->IsString()
        && Handle<String>::cast(key)->AsArrayIndex(&element_index)
        && element_index > max_element_index) {
      max_element_index = element_index;
      elements++;
    } else if (key->IsSmi()) {
      int key_value = Smi::cast(*key)->value();
      if (key_value > 0
          && static_cast<uint32_t>(key_value) > max_element_index) {
        max_element_index = key_value;
      }
      elements++;
    }

    // Add name, value pair to the fixed array.
    constant_properties->set(position++, *key);
    constant_properties->set(position++, *value);
  }
  *fast_elements =
      (max_element_index <= 32) || ((2 * elements) >= max_element_index);
  *is_simple = is_simple_acc;
  *depth = depth_acc;
}


ObjectLiteral::Property* Parser::ParseObjectLiteralGetSet(bool is_getter,
                                                          bool* ok) {
  // Special handling of getter and setter syntax:
  // { ... , get foo() { ... }, ... , set foo(v) { ... v ... } , ... }
  // We have already read the "get" or "set" keyword.
  Token::Value next = Next();
  bool is_keyword = Token::IsKeyword(next);
  if (next == Token::IDENTIFIER || next == Token::NUMBER ||
      next == Token::FUTURE_RESERVED_WORD ||
      next == Token::FUTURE_STRICT_RESERVED_WORD ||
      next == Token::STRING || is_keyword) {
    Handle<String> name;
    if (is_keyword) {
      name = isolate_->factory()->LookupAsciiSymbol(Token::String(next));
    } else {
      name = GetSymbol(CHECK_OK);
    }
    FunctionLiteral* value =
        ParseFunctionLiteral(name,
                             false,   // reserved words are allowed here
                             RelocInfo::kNoPosition,
                             FunctionLiteral::ANONYMOUS_EXPRESSION,
                             CHECK_OK);
    // Allow any number of parameters for compatiabilty with JSC.
    // Specification only allows zero parameters for get and one for set.
    ObjectLiteral::Property* property =
        new(zone()) ObjectLiteral::Property(is_getter, value);
    return property;
  } else {
    ReportUnexpectedToken(next);
    *ok = false;
    return NULL;
  }
}


Expression* Parser::ParseObjectLiteral(bool* ok) {
  // ObjectLiteral ::
  //   '{' (
  //       ((IdentifierName | String | Number) ':' AssignmentExpression)
  //     | (('get' | 'set') (IdentifierName | String | Number) FunctionLiteral)
  //    )*[','] '}'

  ZoneList<ObjectLiteral::Property*>* properties =
      new(zone()) ZoneList<ObjectLiteral::Property*>(4);
  int number_of_boilerplate_properties = 0;
  bool has_function = false;

  ObjectLiteralPropertyChecker checker(this, top_scope_->language_mode());

  Expect(Token::LBRACE, CHECK_OK);

  while (peek() != Token::RBRACE) {
    if (fni_ != NULL) fni_->Enter();

    Literal* key = NULL;
    Token::Value next = peek();

    // Location of the property name token
    Scanner::Location loc = scanner().peek_location();

    switch (next) {
      case Token::FUTURE_RESERVED_WORD:
      case Token::FUTURE_STRICT_RESERVED_WORD:
      case Token::IDENTIFIER: {
        bool is_getter = false;
        bool is_setter = false;
        Handle<String> id =
            ParseIdentifierNameOrGetOrSet(&is_getter, &is_setter, CHECK_OK);
        if (fni_ != NULL) fni_->PushLiteralName(id);

        if ((is_getter || is_setter) && peek() != Token::COLON) {
            // Update loc to point to the identifier
            loc = scanner().peek_location();
            ObjectLiteral::Property* property =
                ParseObjectLiteralGetSet(is_getter, CHECK_OK);
            if (IsBoilerplateProperty(property)) {
              number_of_boilerplate_properties++;
            }
            // Validate the property.
            checker.CheckProperty(property, loc, CHECK_OK);
            properties->Add(property);
            if (peek() != Token::RBRACE) Expect(Token::COMMA, CHECK_OK);

            if (fni_ != NULL) {
              fni_->Infer();
              fni_->Leave();
            }
            continue;  // restart the while
        }
        // Failed to parse as get/set property, so it's just a property
        // called "get" or "set".
        key = NewLiteral(id);
        break;
      }
      case Token::STRING: {
        Consume(Token::STRING);
        Handle<String> string = GetSymbol(CHECK_OK);
        if (fni_ != NULL) fni_->PushLiteralName(string);
        uint32_t index;
        if (!string.is_null() && string->AsArrayIndex(&index)) {
          key = NewNumberLiteral(index);
          break;
        }
        key = NewLiteral(string);
        break;
      }
      case Token::NUMBER: {
        Consume(Token::NUMBER);
        ASSERT(scanner().is_literal_ascii());
        double value = StringToDouble(isolate()->unicode_cache(),
                                      scanner().literal_ascii_string(),
                                      ALLOW_HEX | ALLOW_OCTALS);
        key = NewNumberLiteral(value);
        break;
      }
      default:
        if (Token::IsKeyword(next)) {
          Consume(next);
          Handle<String> string = GetSymbol(CHECK_OK);
          key = NewLiteral(string);
        } else {
          // Unexpected token.
          Token::Value next = Next();
          ReportUnexpectedToken(next);
          *ok = false;
          return NULL;
        }
    }

    Expect(Token::COLON, CHECK_OK);
    Expression* value = ParseAssignmentExpression(true, CHECK_OK);

    ObjectLiteral::Property* property =
        new(zone()) ObjectLiteral::Property(key, value);

    // Mark top-level object literals that contain function literals and
    // pretenure the literal so it can be added as a constant function
    // property.
    if (top_scope_->DeclarationScope()->is_global_scope() &&
        value->AsFunctionLiteral() != NULL) {
      has_function = true;
      value->AsFunctionLiteral()->set_pretenure();
    }

    // Count CONSTANT or COMPUTED properties to maintain the enumeration order.
    if (IsBoilerplateProperty(property)) number_of_boilerplate_properties++;
    // Validate the property
    checker.CheckProperty(property, loc, CHECK_OK);
    properties->Add(property);

    // TODO(1240767): Consider allowing trailing comma.
    if (peek() != Token::RBRACE) Expect(Token::COMMA, CHECK_OK);

    if (fni_ != NULL) {
      fni_->Infer();
      fni_->Leave();
    }
  }
  Expect(Token::RBRACE, CHECK_OK);

  // Computation of literal_index must happen before pre parse bailout.
  int literal_index = current_function_state_->NextMaterializedLiteralIndex();

  Handle<FixedArray> constant_properties = isolate()->factory()->NewFixedArray(
      number_of_boilerplate_properties * 2, TENURED);

  bool is_simple = true;
  bool fast_elements = true;
  int depth = 1;
  BuildObjectLiteralConstantProperties(properties,
                                       constant_properties,
                                       &is_simple,
                                       &fast_elements,
                                       &depth);
  return new(zone()) ObjectLiteral(isolate(),
                                   constant_properties,
                                   properties,
                                   literal_index,
                                   is_simple,
                                   fast_elements,
                                   depth,
                                   has_function);
}


Expression* Parser::ParseRegExpLiteral(bool seen_equal, bool* ok) {
  if (!scanner().ScanRegExpPattern(seen_equal)) {
    Next();
    ReportMessage("unterminated_regexp", Vector<const char*>::empty());
    *ok = false;
    return NULL;
  }

  int literal_index = current_function_state_->NextMaterializedLiteralIndex();

  Handle<String> js_pattern = NextLiteralString(TENURED);
  scanner().ScanRegExpFlags();
  Handle<String> js_flags = NextLiteralString(TENURED);
  Next();

  return new(zone()) RegExpLiteral(
      isolate(), js_pattern, js_flags, literal_index);
}


ZoneList<Expression*>* Parser::ParseArguments(bool* ok) {
  // Arguments ::
  //   '(' (AssignmentExpression)*[','] ')'

  ZoneList<Expression*>* result = new(zone()) ZoneList<Expression*>(4);
  Expect(Token::LPAREN, CHECK_OK);
  bool done = (peek() == Token::RPAREN);
  while (!done) {
    Expression* argument = ParseAssignmentExpression(true, CHECK_OK);
    result->Add(argument);
    if (result->length() > kMaxNumFunctionParameters) {
      ReportMessageAt(scanner().location(), "too_many_arguments",
                      Vector<const char*>::empty());
      *ok = false;
      return NULL;
    }
    done = (peek() == Token::RPAREN);
    if (!done) Expect(Token::COMMA, CHECK_OK);
  }
  Expect(Token::RPAREN, CHECK_OK);
  return result;
}


class SingletonLogger : public ParserRecorder {
 public:
  SingletonLogger() : has_error_(false), start_(-1), end_(-1) { }
  ~SingletonLogger() { }

  void Reset() { has_error_ = false; }

  virtual void LogFunction(int start,
                           int end,
                           int literals,
                           int properties,
                           LanguageMode mode) {
    ASSERT(!has_error_);
    start_ = start;
    end_ = end;
    literals_ = literals;
    properties_ = properties;
    mode_ = mode;
  };

  // Logs a symbol creation of a literal or identifier.
  virtual void LogAsciiSymbol(int start, Vector<const char> literal) { }
  virtual void LogUC16Symbol(int start, Vector<const uc16> literal) { }

  // Logs an error message and marks the log as containing an error.
  // Further logging will be ignored, and ExtractData will return a vector
  // representing the error only.
  virtual void LogMessage(int start,
                          int end,
                          const char* message,
                          const char* argument_opt) {
    has_error_ = true;
    start_ = start;
    end_ = end;
    message_ = message;
    argument_opt_ = argument_opt;
  }

  virtual int function_position() { return 0; }

  virtual int symbol_position() { return 0; }

  virtual int symbol_ids() { return -1; }

  virtual Vector<unsigned> ExtractData() {
    UNREACHABLE();
    return Vector<unsigned>();
  }

  virtual void PauseRecording() { }

  virtual void ResumeRecording() { }

  bool has_error() { return has_error_; }

  int start() { return start_; }
  int end() { return end_; }
  int literals() {
    ASSERT(!has_error_);
    return literals_;
  }
  int properties() {
    ASSERT(!has_error_);
    return properties_;
  }
  LanguageMode language_mode() {
    ASSERT(!has_error_);
    return mode_;
  }
  const char* message() {
    ASSERT(has_error_);
    return message_;
  }
  const char* argument_opt() {
    ASSERT(has_error_);
    return argument_opt_;
  }

 private:
  bool has_error_;
  int start_;
  int end_;
  // For function entries.
  int literals_;
  int properties_;
  LanguageMode mode_;
  // For error messages.
  const char* message_;
  const char* argument_opt_;
};


FunctionLiteral* Parser::ParseFunctionLiteral(Handle<String> function_name,
                                              bool name_is_strict_reserved,
                                              int function_token_position,
                                              FunctionLiteral::Type type,
                                              bool* ok) {
  // Function ::
  //   '(' FormalParameterList? ')' '{' FunctionBody '}'

  // Anonymous functions were passed either the empty symbol or a null
  // handle as the function name.  Remember if we were passed a non-empty
  // handle to decide whether to invoke function name inference.
  bool should_infer_name = function_name.is_null();

  // We want a non-null handle as the function name.
  if (should_infer_name) {
    function_name = isolate()->factory()->empty_symbol();
  }

  int num_parameters = 0;
  // Function declarations are function scoped in normal mode, so they are
  // hoisted. In harmony block scoping mode they are block scoped, so they
  // are not hoisted.
  Scope* scope = (type == FunctionLiteral::DECLARATION && !is_extended_mode())
      ? NewScope(top_scope_->DeclarationScope(), FUNCTION_SCOPE)
      : NewScope(top_scope_, FUNCTION_SCOPE);
  ZoneList<Statement*>* body = NULL;
  int materialized_literal_count = -1;
  int expected_property_count = -1;
  int handler_count = 0;
  bool only_simple_this_property_assignments;
  Handle<FixedArray> this_property_assignments;
  bool has_duplicate_parameters = false;
  // Parse function body.
  { FunctionState function_state(this, scope, isolate());
    top_scope_->SetScopeName(function_name);

    //  FormalParameterList ::
    //    '(' (Identifier)*[','] ')'
    Expect(Token::LPAREN, CHECK_OK);
    scope->set_start_position(scanner().location().beg_pos);
    Scanner::Location name_loc = Scanner::Location::invalid();
    Scanner::Location dupe_loc = Scanner::Location::invalid();
    Scanner::Location reserved_loc = Scanner::Location::invalid();

    bool done = (peek() == Token::RPAREN);
    while (!done) {
      bool is_strict_reserved = false;
      Handle<String> param_name =
          ParseIdentifierOrStrictReservedWord(&is_strict_reserved,
                                              CHECK_OK);

      // Store locations for possible future error reports.
      if (!name_loc.IsValid() && IsEvalOrArguments(param_name)) {
        name_loc = scanner().location();
      }
      if (!dupe_loc.IsValid() && top_scope_->IsDeclared(param_name)) {
        has_duplicate_parameters = true;
        dupe_loc = scanner().location();
      }
      if (!reserved_loc.IsValid() && is_strict_reserved) {
        reserved_loc = scanner().location();
      }

      top_scope_->DeclareParameter(param_name, is_extended_mode() ? LET : VAR);
      num_parameters++;
      if (num_parameters > kMaxNumFunctionParameters) {
        ReportMessageAt(scanner().location(), "too_many_parameters",
                        Vector<const char*>::empty());
        *ok = false;
        return NULL;
      }
      done = (peek() == Token::RPAREN);
      if (!done) Expect(Token::COMMA, CHECK_OK);
    }
    Expect(Token::RPAREN, CHECK_OK);

    Expect(Token::LBRACE, CHECK_OK);

    // If we have a named function expression, we add a local variable
    // declaration to the body of the function with the name of the
    // function and let it refer to the function itself (closure).
    // NOTE: We create a proxy and resolve it here so that in the
    // future we can change the AST to only refer to VariableProxies
    // instead of Variables and Proxis as is the case now.
    Variable* fvar = NULL;
    Token::Value fvar_init_op = Token::INIT_CONST;
    if (type == FunctionLiteral::NAMED_EXPRESSION) {
      VariableMode fvar_mode;
      if (is_extended_mode()) {
        fvar_mode = CONST_HARMONY;
        fvar_init_op = Token::INIT_CONST_HARMONY;
      } else {
        fvar_mode = CONST;
      }
      fvar = top_scope_->DeclareFunctionVar(function_name, fvar_mode);
    }

    // Determine whether the function will be lazily compiled.
    // The heuristics are:
    // - It must not have been prohibited by the caller to Parse (some callers
    //   need a full AST).
    // - The outer scope must be trivial (only global variables in scope).
    // - The function mustn't be a function expression with an open parenthesis
    //   before; we consider that a hint that the function will be called
    //   immediately, and it would be a waste of time to make it lazily
    //   compiled.
    // These are all things we can know at this point, without looking at the
    // function itself.
    bool is_lazily_compiled = (mode() == PARSE_LAZILY &&
                               top_scope_->outer_scope()->is_global_scope() &&
                               top_scope_->HasTrivialOuterContext() &&
                               !parenthesized_function_);
    parenthesized_function_ = false;  // The bit was set for this function only.

    if (is_lazily_compiled) {
      int function_block_pos = scanner().location().beg_pos;
      FunctionEntry entry;
      if (pre_data_ != NULL) {
        // If we have pre_data_, we use it to skip parsing the function body.
        // the preparser data contains the information we need to construct the
        // lazy function.
        entry = pre_data()->GetFunctionEntry(function_block_pos);
        if (entry.is_valid()) {
          if (entry.end_pos() <= function_block_pos) {
            // End position greater than end of stream is safe, and hard
            // to check.
            ReportInvalidPreparseData(function_name, CHECK_OK);
          }
          scanner().SeekForward(entry.end_pos() - 1);

          scope->set_end_position(entry.end_pos());
          Expect(Token::RBRACE, CHECK_OK);
          isolate()->counters()->total_preparse_skipped()->Increment(
              scope->end_position() - function_block_pos);
          materialized_literal_count = entry.literal_count();
          expected_property_count = entry.property_count();
          top_scope_->SetLanguageMode(entry.language_mode());
          only_simple_this_property_assignments = false;
          this_property_assignments = isolate()->factory()->empty_fixed_array();
        } else {
          is_lazily_compiled = false;
        }
      } else {
        // With no preparser data, we partially parse the function, without
        // building an AST. This gathers the data needed to build a lazy
        // function.
        SingletonLogger logger;
        preparser::PreParser::PreParseResult result =
            LazyParseFunctionLiteral(&logger);
        if (result == preparser::PreParser::kPreParseStackOverflow) {
          // Propagate stack overflow.
          stack_overflow_ = true;
          *ok = false;
          return NULL;
        }
        if (logger.has_error()) {
          const char* arg = logger.argument_opt();
          Vector<const char*> args;
          if (arg != NULL) {
            args = Vector<const char*>(&arg, 1);
          }
          ReportMessageAt(Scanner::Location(logger.start(), logger.end()),
                          logger.message(), args);
          *ok = false;
          return NULL;
        }
        scope->set_end_position(logger.end());
        Expect(Token::RBRACE, CHECK_OK);
        isolate()->counters()->total_preparse_skipped()->Increment(
            scope->end_position() - function_block_pos);
        materialized_literal_count = logger.literals();
        expected_property_count = logger.properties();
        top_scope_->SetLanguageMode(logger.language_mode());
        only_simple_this_property_assignments = false;
        this_property_assignments = isolate()->factory()->empty_fixed_array();
      }
    }

    if (!is_lazily_compiled) {
      body = new(zone()) ZoneList<Statement*>(8);
      if (fvar != NULL) {
        VariableProxy* fproxy = top_scope_->NewUnresolved(function_name);
        fproxy->BindTo(fvar);
        body->Add(new(zone()) ExpressionStatement(
            new(zone()) Assignment(isolate(),
                                   fvar_init_op,
                                   fproxy,
                                   new(zone()) ThisFunction(isolate()),
                                   RelocInfo::kNoPosition)));
      }
      ParseSourceElements(body, Token::RBRACE, CHECK_OK);

      materialized_literal_count = function_state.materialized_literal_count();
      expected_property_count = function_state.expected_property_count();
      handler_count = function_state.handler_count();
      only_simple_this_property_assignments =
          function_state.only_simple_this_property_assignments();
      this_property_assignments = function_state.this_property_assignments();

      Expect(Token::RBRACE, CHECK_OK);
      scope->set_end_position(scanner().location().end_pos);
    }

    // Validate strict mode.
    if (!top_scope_->is_classic_mode()) {
      if (IsEvalOrArguments(function_name)) {
        int start_pos = scope->start_position();
        int position = function_token_position != RelocInfo::kNoPosition
            ? function_token_position
            : (start_pos > 0 ? start_pos - 1 : start_pos);
        Scanner::Location location = Scanner::Location(position, start_pos);
        ReportMessageAt(location,
                        "strict_function_name", Vector<const char*>::empty());
        *ok = false;
        return NULL;
      }
      if (name_loc.IsValid()) {
        ReportMessageAt(name_loc, "strict_param_name",
                        Vector<const char*>::empty());
        *ok = false;
        return NULL;
      }
      if (dupe_loc.IsValid()) {
        ReportMessageAt(dupe_loc, "strict_param_dupe",
                        Vector<const char*>::empty());
        *ok = false;
        return NULL;
      }
      if (name_is_strict_reserved) {
        int start_pos = scope->start_position();
        int position = function_token_position != RelocInfo::kNoPosition
            ? function_token_position
            : (start_pos > 0 ? start_pos - 1 : start_pos);
        Scanner::Location location = Scanner::Location(position, start_pos);
        ReportMessageAt(location, "strict_reserved_word",
                        Vector<const char*>::empty());
        *ok = false;
        return NULL;
      }
      if (reserved_loc.IsValid()) {
        ReportMessageAt(reserved_loc, "strict_reserved_word",
                        Vector<const char*>::empty());
        *ok = false;
        return NULL;
      }
      CheckOctalLiteral(scope->start_position(),
                        scope->end_position(),
                        CHECK_OK);
    }
  }

  if (is_extended_mode()) {
    CheckConflictingVarDeclarations(scope, CHECK_OK);
  }

  FunctionLiteral* function_literal =
      new(zone()) FunctionLiteral(isolate(),
                                  function_name,
                                  scope,
                                  body,
                                  materialized_literal_count,
                                  expected_property_count,
                                  handler_count,
                                  only_simple_this_property_assignments,
                                  this_property_assignments,
                                  num_parameters,
                                  type,
                                  has_duplicate_parameters);
  function_literal->set_function_token_position(function_token_position);

  if (fni_ != NULL && should_infer_name) fni_->AddFunction(function_literal);
  return function_literal;
}


preparser::PreParser::PreParseResult Parser::LazyParseFunctionLiteral(
    SingletonLogger* logger) {
  HistogramTimerScope preparse_scope(isolate()->counters()->pre_parse());
  ASSERT_EQ(Token::LBRACE, scanner().current_token());

  if (reusable_preparser_ == NULL) {
    intptr_t stack_limit = isolate()->stack_guard()->real_climit();
    bool do_allow_lazy = true;
    reusable_preparser_ = new preparser::PreParser(&scanner_,
                                                   NULL,
                                                   stack_limit,
                                                   do_allow_lazy,
                                                   allow_natives_syntax_);
  }
  preparser::PreParser::PreParseResult result =
      reusable_preparser_->PreParseLazyFunction(top_scope_->language_mode(),
                                                logger);
  return result;
}


Expression* Parser::ParseV8Intrinsic(bool* ok) {
  // CallRuntime ::
  //   '%' Identifier Arguments

  Expect(Token::MOD, CHECK_OK);
  Handle<String> name = ParseIdentifier(CHECK_OK);
  ZoneList<Expression*>* args = ParseArguments(CHECK_OK);

  if (extension_ != NULL) {
    // The extension structures are only accessible while parsing the
    // very first time not when reparsing because of lazy compilation.
    top_scope_->DeclarationScope()->ForceEagerCompilation();
  }

  const Runtime::Function* function = Runtime::FunctionForSymbol(name);

  // Check for built-in IS_VAR macro.
  if (function != NULL &&
      function->intrinsic_type == Runtime::RUNTIME &&
      function->function_id == Runtime::kIS_VAR) {
    // %IS_VAR(x) evaluates to x if x is a variable,
    // leads to a parse error otherwise.  Could be implemented as an
    // inline function %_IS_VAR(x) to eliminate this special case.
    if (args->length() == 1 && args->at(0)->AsVariableProxy() != NULL) {
      return args->at(0);
    } else {
      ReportMessage("unable_to_parse", Vector<const char*>::empty());
      *ok = false;
      return NULL;
    }
  }

  // Check that the expected number of arguments are being passed.
  if (function != NULL &&
      function->nargs != -1 &&
      function->nargs != args->length()) {
    ReportMessage("illegal_access", Vector<const char*>::empty());
    *ok = false;
    return NULL;
  }

  // We have a valid intrinsics call or a call to a builtin.
  return new(zone()) CallRuntime(isolate(), name, function, args);
}


bool Parser::peek_any_identifier() {
  Token::Value next = peek();
  return next == Token::IDENTIFIER ||
         next == Token::FUTURE_RESERVED_WORD ||
         next == Token::FUTURE_STRICT_RESERVED_WORD;
}


void Parser::Consume(Token::Value token) {
  Token::Value next = Next();
  USE(next);
  USE(token);
  ASSERT(next == token);
}


void Parser::Expect(Token::Value token, bool* ok) {
  Token::Value next = Next();
  if (next == token) return;
  ReportUnexpectedToken(next);
  *ok = false;
}


bool Parser::Check(Token::Value token) {
  Token::Value next = peek();
  if (next == token) {
    Consume(next);
    return true;
  }
  return false;
}


void Parser::ExpectSemicolon(bool* ok) {
  // Check for automatic semicolon insertion according to
  // the rules given in ECMA-262, section 7.9, page 21.
  Token::Value tok = peek();
  if (tok == Token::SEMICOLON) {
    Next();
    return;
  }
  if (scanner().HasAnyLineTerminatorBeforeNext() ||
      tok == Token::RBRACE ||
      tok == Token::EOS) {
    return;
  }
  Expect(Token::SEMICOLON, ok);
}


Literal* Parser::GetLiteralUndefined() {
  return NewLiteral(isolate()->factory()->undefined_value());
}


Literal* Parser::GetLiteralTheHole() {
  return NewLiteral(isolate()->factory()->the_hole_value());
}


Literal* Parser::GetLiteralNumber(double value) {
  return NewNumberLiteral(value);
}


// Parses an identifier that is valid for the current scope, in particular it
// fails on strict mode future reserved keywords in a strict scope.
Handle<String> Parser::ParseIdentifier(bool* ok) {
  if (!top_scope_->is_classic_mode()) {
    Expect(Token::IDENTIFIER, ok);
  } else if (!Check(Token::IDENTIFIER)) {
    Expect(Token::FUTURE_STRICT_RESERVED_WORD, ok);
  }
  if (!*ok) return Handle<String>();
  return GetSymbol(ok);
}


// Parses and identifier or a strict mode future reserved word, and indicate
// whether it is strict mode future reserved.
Handle<String> Parser::ParseIdentifierOrStrictReservedWord(
    bool* is_strict_reserved, bool* ok) {
  *is_strict_reserved = false;
  if (!Check(Token::IDENTIFIER)) {
    Expect(Token::FUTURE_STRICT_RESERVED_WORD, ok);
    *is_strict_reserved = true;
  }
  if (!*ok) return Handle<String>();
  return GetSymbol(ok);
}


Handle<String> Parser::ParseIdentifierName(bool* ok) {
  Token::Value next = Next();
  if (next != Token::IDENTIFIER &&
      next != Token::FUTURE_RESERVED_WORD &&
      next != Token::FUTURE_STRICT_RESERVED_WORD &&
      !Token::IsKeyword(next)) {
    ReportUnexpectedToken(next);
    *ok = false;
    return Handle<String>();
  }
  return GetSymbol(ok);
}


// Checks LHS expression for assignment and prefix/postfix increment/decrement
// in strict mode.
void Parser::CheckStrictModeLValue(Expression* expression,
                                   const char* error,
                                   bool* ok) {
  ASSERT(!top_scope_->is_classic_mode());
  VariableProxy* lhs = expression != NULL
      ? expression->AsVariableProxy()
      : NULL;

  if (lhs != NULL && !lhs->is_this() && IsEvalOrArguments(lhs->name())) {
    ReportMessage(error, Vector<const char*>::empty());
    *ok = false;
  }
}


// Checks whether an octal literal was last seen between beg_pos and end_pos.
// If so, reports an error. Only called for strict mode.
void Parser::CheckOctalLiteral(int beg_pos, int end_pos, bool* ok) {
  Scanner::Location octal = scanner().octal_position();
  if (octal.IsValid() &&
      beg_pos <= octal.beg_pos &&
      octal.end_pos <= end_pos) {
    ReportMessageAt(octal, "strict_octal_literal",
                    Vector<const char*>::empty());
    scanner().clear_octal_position();
    *ok = false;
  }
}


void Parser::CheckConflictingVarDeclarations(Scope* scope, bool* ok) {
  Declaration* decl = scope->CheckConflictingVarDeclarations();
  if (decl != NULL) {
    // In harmony mode we treat conflicting variable bindinds as early
    // errors. See ES5 16 for a definition of early errors.
    Handle<String> name = decl->proxy()->name();
    SmartArrayPointer<char> c_string = name->ToCString(DISALLOW_NULLS);
    const char* elms[2] = { "Variable", *c_string };
    Vector<const char*> args(elms, 2);
    int position = decl->proxy()->position();
    Scanner::Location location = position == RelocInfo::kNoPosition
        ? Scanner::Location::invalid()
        : Scanner::Location(position, position + 1);
    ReportMessageAt(location, "redeclaration", args);
    *ok = false;
  }
}


// This function reads an identifier name and determines whether or not it
// is 'get' or 'set'.
Handle<String> Parser::ParseIdentifierNameOrGetOrSet(bool* is_get,
                                                     bool* is_set,
                                                     bool* ok) {
  Handle<String> result = ParseIdentifierName(ok);
  if (!*ok) return Handle<String>();
  if (scanner().is_literal_ascii() && scanner().literal_length() == 3) {
    const char* token = scanner().literal_ascii_string().start();
    *is_get = strncmp(token, "get", 3) == 0;
    *is_set = !*is_get && strncmp(token, "set", 3) == 0;
  }
  return result;
}


// ----------------------------------------------------------------------------
// Parser support


bool Parser::TargetStackContainsLabel(Handle<String> label) {
  for (Target* t = target_stack_; t != NULL; t = t->previous()) {
    BreakableStatement* stat = t->node()->AsBreakableStatement();
    if (stat != NULL && ContainsLabel(stat->labels(), label))
      return true;
  }
  return false;
}


BreakableStatement* Parser::LookupBreakTarget(Handle<String> label, bool* ok) {
  bool anonymous = label.is_null();
  for (Target* t = target_stack_; t != NULL; t = t->previous()) {
    BreakableStatement* stat = t->node()->AsBreakableStatement();
    if (stat == NULL) continue;
    if ((anonymous && stat->is_target_for_anonymous()) ||
        (!anonymous && ContainsLabel(stat->labels(), label))) {
      RegisterTargetUse(stat->break_target(), t->previous());
      return stat;
    }
  }
  return NULL;
}


IterationStatement* Parser::LookupContinueTarget(Handle<String> label,
                                                 bool* ok) {
  bool anonymous = label.is_null();
  for (Target* t = target_stack_; t != NULL; t = t->previous()) {
    IterationStatement* stat = t->node()->AsIterationStatement();
    if (stat == NULL) continue;

    ASSERT(stat->is_target_for_anonymous());
    if (anonymous || ContainsLabel(stat->labels(), label)) {
      RegisterTargetUse(stat->continue_target(), t->previous());
      return stat;
    }
  }
  return NULL;
}


void Parser::RegisterTargetUse(Label* target, Target* stop) {
  // Register that a break target found at the given stop in the
  // target stack has been used from the top of the target stack. Add
  // the break target to any TargetCollectors passed on the stack.
  for (Target* t = target_stack_; t != stop; t = t->previous()) {
    TargetCollector* collector = t->node()->AsTargetCollector();
    if (collector != NULL) collector->AddTarget(target);
  }
}


Literal* Parser::NewNumberLiteral(double number) {
  return NewLiteral(isolate()->factory()->NewNumber(number, TENURED));
}


Expression* Parser::NewThrowReferenceError(Handle<String> type) {
  return NewThrowError(isolate()->factory()->MakeReferenceError_symbol(),
                       type, HandleVector<Object>(NULL, 0));
}


Expression* Parser::NewThrowSyntaxError(Handle<String> type,
                                        Handle<Object> first) {
  int argc = first.is_null() ? 0 : 1;
  Vector< Handle<Object> > arguments = HandleVector<Object>(&first, argc);
  return NewThrowError(
      isolate()->factory()->MakeSyntaxError_symbol(), type, arguments);
}


Expression* Parser::NewThrowTypeError(Handle<String> type,
                                      Handle<Object> first,
                                      Handle<Object> second) {
  ASSERT(!first.is_null() && !second.is_null());
  Handle<Object> elements[] = { first, second };
  Vector< Handle<Object> > arguments =
      HandleVector<Object>(elements, ARRAY_SIZE(elements));
  return NewThrowError(
      isolate()->factory()->MakeTypeError_symbol(), type, arguments);
}


Expression* Parser::NewThrowError(Handle<String> constructor,
                                  Handle<String> type,
                                  Vector< Handle<Object> > arguments) {
  int argc = arguments.length();
  Handle<FixedArray> elements = isolate()->factory()->NewFixedArray(argc,
                                                                    TENURED);
  for (int i = 0; i < argc; i++) {
    Handle<Object> element = arguments[i];
    if (!element.is_null()) {
      elements->set(i, *element);
    }
  }
  Handle<JSArray> array = isolate()->factory()->NewJSArrayWithElements(elements,
                                                                       TENURED);

  ZoneList<Expression*>* args = new(zone()) ZoneList<Expression*>(2);
  args->Add(NewLiteral(type));
  args->Add(NewLiteral(array));
  CallRuntime* call_constructor = new(zone()) CallRuntime(isolate(),
                                                          constructor,
                                                          NULL,
                                                          args);
  return new(zone()) Throw(isolate(),
                           call_constructor,
                           scanner().location().beg_pos);
}

// ----------------------------------------------------------------------------
// Regular expressions


RegExpParser::RegExpParser(FlatStringReader* in,
                           Handle<String>* error,
                           bool multiline)
    : isolate_(Isolate::Current()),
      error_(error),
      captures_(NULL),
      in_(in),
      current_(kEndMarker),
      next_pos_(0),
      capture_count_(0),
      has_more_(true),
      multiline_(multiline),
      simple_(false),
      contains_anchor_(false),
      is_scanned_for_captures_(false),
      failed_(false) {
  Advance();
}


uc32 RegExpParser::Next() {
  if (has_next()) {
    return in()->Get(next_pos_);
  } else {
    return kEndMarker;
  }
}


void RegExpParser::Advance() {
  if (next_pos_ < in()->length()) {
    StackLimitCheck check(isolate());
    if (check.HasOverflowed()) {
      ReportError(CStrVector(Isolate::kStackOverflowMessage));
    } else if (isolate()->zone()->excess_allocation()) {
      ReportError(CStrVector("Regular expression too large"));
    } else {
      current_ = in()->Get(next_pos_);
      next_pos_++;
    }
  } else {
    current_ = kEndMarker;
    has_more_ = false;
  }
}


void RegExpParser::Reset(int pos) {
  next_pos_ = pos;
  Advance();
}


void RegExpParser::Advance(int dist) {
  next_pos_ += dist - 1;
  Advance();
}


bool RegExpParser::simple() {
  return simple_;
}

RegExpTree* RegExpParser::ReportError(Vector<const char> message) {
  failed_ = true;
  *error_ = isolate()->factory()->NewStringFromAscii(message, NOT_TENURED);
  // Zip to the end to make sure the no more input is read.
  current_ = kEndMarker;
  next_pos_ = in()->length();
  return NULL;
}


// Pattern ::
//   Disjunction
RegExpTree* RegExpParser::ParsePattern() {
  RegExpTree* result = ParseDisjunction(CHECK_FAILED);
  ASSERT(!has_more());
  // If the result of parsing is a literal string atom, and it has the
  // same length as the input, then the atom is identical to the input.
  if (result->IsAtom() && result->AsAtom()->length() == in()->length()) {
    simple_ = true;
  }
  return result;
}


// Disjunction ::
//   Alternative
//   Alternative | Disjunction
// Alternative ::
//   [empty]
//   Term Alternative
// Term ::
//   Assertion
//   Atom
//   Atom Quantifier
RegExpTree* RegExpParser::ParseDisjunction() {
  // Used to store current state while parsing subexpressions.
  RegExpParserState initial_state(NULL, INITIAL, 0);
  RegExpParserState* stored_state = &initial_state;
  // Cache the builder in a local variable for quick access.
  RegExpBuilder* builder = initial_state.builder();
  while (true) {
    switch (current()) {
    case kEndMarker:
      if (stored_state->IsSubexpression()) {
        // Inside a parenthesized group when hitting end of input.
        ReportError(CStrVector("Unterminated group") CHECK_FAILED);
      }
      ASSERT_EQ(INITIAL, stored_state->group_type());
      // Parsing completed successfully.
      return builder->ToRegExp();
    case ')': {
      if (!stored_state->IsSubexpression()) {
        ReportError(CStrVector("Unmatched ')'") CHECK_FAILED);
      }
      ASSERT_NE(INITIAL, stored_state->group_type());

      Advance();
      // End disjunction parsing and convert builder content to new single
      // regexp atom.
      RegExpTree* body = builder->ToRegExp();

      int end_capture_index = captures_started();

      int capture_index = stored_state->capture_index();
      SubexpressionType type = stored_state->group_type();

      // Restore previous state.
      stored_state = stored_state->previous_state();
      builder = stored_state->builder();

      // Build result of subexpression.
      if (type == CAPTURE) {
        RegExpCapture* capture = new(zone()) RegExpCapture(body, capture_index);
        captures_->at(capture_index - 1) = capture;
        body = capture;
      } else if (type != GROUPING) {
        ASSERT(type == POSITIVE_LOOKAHEAD || type == NEGATIVE_LOOKAHEAD);
        bool is_positive = (type == POSITIVE_LOOKAHEAD);
        body = new(zone()) RegExpLookahead(body,
                                   is_positive,
                                   end_capture_index - capture_index,
                                   capture_index);
      }
      builder->AddAtom(body);
      // For compatability with JSC and ES3, we allow quantifiers after
      // lookaheads, and break in all cases.
      break;
    }
    case '|': {
      Advance();
      builder->NewAlternative();
      continue;
    }
    case '*':
    case '+':
    case '?':
      return ReportError(CStrVector("Nothing to repeat"));
    case '^': {
      Advance();
      if (multiline_) {
        builder->AddAssertion(
            new(zone()) RegExpAssertion(RegExpAssertion::START_OF_LINE));
      } else {
        builder->AddAssertion(
            new(zone()) RegExpAssertion(RegExpAssertion::START_OF_INPUT));
        set_contains_anchor();
      }
      continue;
    }
    case '$': {
      Advance();
      RegExpAssertion::Type type =
          multiline_ ? RegExpAssertion::END_OF_LINE :
                       RegExpAssertion::END_OF_INPUT;
      builder->AddAssertion(new(zone()) RegExpAssertion(type));
      continue;
    }
    case '.': {
      Advance();
      // everything except \x0a, \x0d, \u2028 and \u2029
      ZoneList<CharacterRange>* ranges =
          new(zone()) ZoneList<CharacterRange>(2);
      CharacterRange::AddClassEscape('.', ranges);
      RegExpTree* atom = new(zone()) RegExpCharacterClass(ranges, false);
      builder->AddAtom(atom);
      break;
    }
    case '(': {
      SubexpressionType type = CAPTURE;
      Advance();
      if (current() == '?') {
        switch (Next()) {
          case ':':
            type = GROUPING;
            break;
          case '=':
            type = POSITIVE_LOOKAHEAD;
            break;
          case '!':
            type = NEGATIVE_LOOKAHEAD;
            break;
          default:
            ReportError(CStrVector("Invalid group") CHECK_FAILED);
            break;
        }
        Advance(2);
      } else {
        if (captures_ == NULL) {
          captures_ = new(zone()) ZoneList<RegExpCapture*>(2);
        }
        if (captures_started() >= kMaxCaptures) {
          ReportError(CStrVector("Too many captures") CHECK_FAILED);
        }
        captures_->Add(NULL);
      }
      // Store current state and begin new disjunction parsing.
      stored_state = new(zone()) RegExpParserState(stored_state,
                                           type,
                                           captures_started());
      builder = stored_state->builder();
      continue;
    }
    case '[': {
      RegExpTree* atom = ParseCharacterClass(CHECK_FAILED);
      builder->AddAtom(atom);
      break;
    }
    // Atom ::
    //   \ AtomEscape
    case '\\':
      switch (Next()) {
      case kEndMarker:
        return ReportError(CStrVector("\\ at end of pattern"));
      case 'b':
        Advance(2);
        builder->AddAssertion(
            new(zone()) RegExpAssertion(RegExpAssertion::BOUNDARY));
        continue;
      case 'B':
        Advance(2);
        builder->AddAssertion(
            new(zone()) RegExpAssertion(RegExpAssertion::NON_BOUNDARY));
        continue;
      // AtomEscape ::
      //   CharacterClassEscape
      //
      // CharacterClassEscape :: one of
      //   d D s S w W
      case 'd': case 'D': case 's': case 'S': case 'w': case 'W': {
        uc32 c = Next();
        Advance(2);
        ZoneList<CharacterRange>* ranges =
            new(zone()) ZoneList<CharacterRange>(2);
        CharacterRange::AddClassEscape(c, ranges);
        RegExpTree* atom = new(zone()) RegExpCharacterClass(ranges, false);
        builder->AddAtom(atom);
        break;
      }
      case '1': case '2': case '3': case '4': case '5': case '6':
      case '7': case '8': case '9': {
        int index = 0;
        if (ParseBackReferenceIndex(&index)) {
          RegExpCapture* capture = NULL;
          if (captures_ != NULL && index <= captures_->length()) {
            capture = captures_->at(index - 1);
          }
          if (capture == NULL) {
            builder->AddEmpty();
            break;
          }
          RegExpTree* atom = new(zone()) RegExpBackReference(capture);
          builder->AddAtom(atom);
          break;
        }
        uc32 first_digit = Next();
        if (first_digit == '8' || first_digit == '9') {
          // Treat as identity escape
          builder->AddCharacter(first_digit);
          Advance(2);
          break;
        }
      }
      // FALLTHROUGH
      case '0': {
        Advance();
        uc32 octal = ParseOctalLiteral();
        builder->AddCharacter(octal);
        break;
      }
      // ControlEscape :: one of
      //   f n r t v
      case 'f':
        Advance(2);
        builder->AddCharacter('\f');
        break;
      case 'n':
        Advance(2);
        builder->AddCharacter('\n');
        break;
      case 'r':
        Advance(2);
        builder->AddCharacter('\r');
        break;
      case 't':
        Advance(2);
        builder->AddCharacter('\t');
        break;
      case 'v':
        Advance(2);
        builder->AddCharacter('\v');
        break;
      case 'c': {
        Advance();
        uc32 controlLetter = Next();
        // Special case if it is an ASCII letter.
        // Convert lower case letters to uppercase.
        uc32 letter = controlLetter & ~('a' ^ 'A');
        if (letter < 'A' || 'Z' < letter) {
          // controlLetter is not in range 'A'-'Z' or 'a'-'z'.
          // This is outside the specification. We match JSC in
          // reading the backslash as a literal character instead
          // of as starting an escape.
          builder->AddCharacter('\\');
        } else {
          Advance(2);
          builder->AddCharacter(controlLetter & 0x1f);
        }
        break;
      }
      case 'x': {
        Advance(2);
        uc32 value;
        if (ParseHexEscape(2, &value)) {
          builder->AddCharacter(value);
        } else {
          builder->AddCharacter('x');
        }
        break;
      }
      case 'u': {
        Advance(2);
        uc32 value;
        if (ParseHexEscape(4, &value)) {
          builder->AddCharacter(value);
        } else {
          builder->AddCharacter('u');
        }
        break;
      }
      default:
        // Identity escape.
        builder->AddCharacter(Next());
        Advance(2);
        break;
      }
      break;
    case '{': {
      int dummy;
      if (ParseIntervalQuantifier(&dummy, &dummy)) {
        ReportError(CStrVector("Nothing to repeat") CHECK_FAILED);
      }
      // fallthrough
    }
    default:
      builder->AddCharacter(current());
      Advance();
      break;
    }  // end switch(current())

    int min;
    int max;
    switch (current()) {
    // QuantifierPrefix ::
    //   *
    //   +
    //   ?
    //   {
    case '*':
      min = 0;
      max = RegExpTree::kInfinity;
      Advance();
      break;
    case '+':
      min = 1;
      max = RegExpTree::kInfinity;
      Advance();
      break;
    case '?':
      min = 0;
      max = 1;
      Advance();
      break;
    case '{':
      if (ParseIntervalQuantifier(&min, &max)) {
        if (max < min) {
          ReportError(CStrVector("numbers out of order in {} quantifier.")
                      CHECK_FAILED);
        }
        break;
      } else {
        continue;
      }
    default:
      continue;
    }
    RegExpQuantifier::Type type = RegExpQuantifier::GREEDY;
    if (current() == '?') {
      type = RegExpQuantifier::NON_GREEDY;
      Advance();
    } else if (FLAG_regexp_possessive_quantifier && current() == '+') {
      // FLAG_regexp_possessive_quantifier is a debug-only flag.
      type = RegExpQuantifier::POSSESSIVE;
      Advance();
    }
    builder->AddQuantifierToAtom(min, max, type);
  }
}


#ifdef DEBUG
// Currently only used in an ASSERT.
static bool IsSpecialClassEscape(uc32 c) {
  switch (c) {
    case 'd': case 'D':
    case 's': case 'S':
    case 'w': case 'W':
      return true;
    default:
      return false;
  }
}
#endif


// In order to know whether an escape is a backreference or not we have to scan
// the entire regexp and find the number of capturing parentheses.  However we
// don't want to scan the regexp twice unless it is necessary.  This mini-parser
// is called when needed.  It can see the difference between capturing and
// noncapturing parentheses and can skip character classes and backslash-escaped
// characters.
void RegExpParser::ScanForCaptures() {
  // Start with captures started previous to current position
  int capture_count = captures_started();
  // Add count of captures after this position.
  int n;
  while ((n = current()) != kEndMarker) {
    Advance();
    switch (n) {
      case '\\':
        Advance();
        break;
      case '[': {
        int c;
        while ((c = current()) != kEndMarker) {
          Advance();
          if (c == '\\') {
            Advance();
          } else {
            if (c == ']') break;
          }
        }
        break;
      }
      case '(':
        if (current() != '?') capture_count++;
        break;
    }
  }
  capture_count_ = capture_count;
  is_scanned_for_captures_ = true;
}


bool RegExpParser::ParseBackReferenceIndex(int* index_out) {
  ASSERT_EQ('\\', current());
  ASSERT('1' <= Next() && Next() <= '9');
  // Try to parse a decimal literal that is no greater than the total number
  // of left capturing parentheses in the input.
  int start = position();
  int value = Next() - '0';
  Advance(2);
  while (true) {
    uc32 c = current();
    if (IsDecimalDigit(c)) {
      value = 10 * value + (c - '0');
      if (value > kMaxCaptures) {
        Reset(start);
        return false;
      }
      Advance();
    } else {
      break;
    }
  }
  if (value > captures_started()) {
    if (!is_scanned_for_captures_) {
      int saved_position = position();
      ScanForCaptures();
      Reset(saved_position);
    }
    if (value > capture_count_) {
      Reset(start);
      return false;
    }
  }
  *index_out = value;
  return true;
}


// QuantifierPrefix ::
//   { DecimalDigits }
//   { DecimalDigits , }
//   { DecimalDigits , DecimalDigits }
//
// Returns true if parsing succeeds, and set the min_out and max_out
// values. Values are truncated to RegExpTree::kInfinity if they overflow.
bool RegExpParser::ParseIntervalQuantifier(int* min_out, int* max_out) {
  ASSERT_EQ(current(), '{');
  int start = position();
  Advance();
  int min = 0;
  if (!IsDecimalDigit(current())) {
    Reset(start);
    return false;
  }
  while (IsDecimalDigit(current())) {
    int next = current() - '0';
    if (min > (RegExpTree::kInfinity - next) / 10) {
      // Overflow. Skip past remaining decimal digits and return -1.
      do {
        Advance();
      } while (IsDecimalDigit(current()));
      min = RegExpTree::kInfinity;
      break;
    }
    min = 10 * min + next;
    Advance();
  }
  int max = 0;
  if (current() == '}') {
    max = min;
    Advance();
  } else if (current() == ',') {
    Advance();
    if (current() == '}') {
      max = RegExpTree::kInfinity;
      Advance();
    } else {
      while (IsDecimalDigit(current())) {
        int next = current() - '0';
        if (max > (RegExpTree::kInfinity - next) / 10) {
          do {
            Advance();
          } while (IsDecimalDigit(current()));
          max = RegExpTree::kInfinity;
          break;
        }
        max = 10 * max + next;
        Advance();
      }
      if (current() != '}') {
        Reset(start);
        return false;
      }
      Advance();
    }
  } else {
    Reset(start);
    return false;
  }
  *min_out = min;
  *max_out = max;
  return true;
}


uc32 RegExpParser::ParseOctalLiteral() {
  ASSERT('0' <= current() && current() <= '7');
  // For compatibility with some other browsers (not all), we parse
  // up to three octal digits with a value below 256.
  uc32 value = current() - '0';
  Advance();
  if ('0' <= current() && current() <= '7') {
    value = value * 8 + current() - '0';
    Advance();
    if (value < 32 && '0' <= current() && current() <= '7') {
      value = value * 8 + current() - '0';
      Advance();
    }
  }
  return value;
}


bool RegExpParser::ParseHexEscape(int length, uc32 *value) {
  int start = position();
  uc32 val = 0;
  bool done = false;
  for (int i = 0; !done; i++) {
    uc32 c = current();
    int d = HexValue(c);
    if (d < 0) {
      Reset(start);
      return false;
    }
    val = val * 16 + d;
    Advance();
    if (i == length - 1) {
      done = true;
    }
  }
  *value = val;
  return true;
}


uc32 RegExpParser::ParseClassCharacterEscape() {
  ASSERT(current() == '\\');
  ASSERT(has_next() && !IsSpecialClassEscape(Next()));
  Advance();
  switch (current()) {
    case 'b':
      Advance();
      return '\b';
    // ControlEscape :: one of
    //   f n r t v
    case 'f':
      Advance();
      return '\f';
    case 'n':
      Advance();
      return '\n';
    case 'r':
      Advance();
      return '\r';
    case 't':
      Advance();
      return '\t';
    case 'v':
      Advance();
      return '\v';
    case 'c': {
      uc32 controlLetter = Next();
      uc32 letter = controlLetter & ~('A' ^ 'a');
      // For compatibility with JSC, inside a character class
      // we also accept digits and underscore as control characters.
      if ((controlLetter >= '0' && controlLetter <= '9') ||
          controlLetter == '_' ||
          (letter >= 'A' && letter <= 'Z')) {
        Advance(2);
        // Control letters mapped to ASCII control characters in the range
        // 0x00-0x1f.
        return controlLetter & 0x1f;
      }
      // We match JSC in reading the backslash as a literal
      // character instead of as starting an escape.
      return '\\';
    }
    case '0': case '1': case '2': case '3': case '4': case '5':
    case '6': case '7':
      // For compatibility, we interpret a decimal escape that isn't
      // a back reference (and therefore either \0 or not valid according
      // to the specification) as a 1..3 digit octal character code.
      return ParseOctalLiteral();
    case 'x': {
      Advance();
      uc32 value;
      if (ParseHexEscape(2, &value)) {
        return value;
      }
      // If \x is not followed by a two-digit hexadecimal, treat it
      // as an identity escape.
      return 'x';
    }
    case 'u': {
      Advance();
      uc32 value;
      if (ParseHexEscape(4, &value)) {
        return value;
      }
      // If \u is not followed by a four-digit hexadecimal, treat it
      // as an identity escape.
      return 'u';
    }
    default: {
      // Extended identity escape. We accept any character that hasn't
      // been matched by a more specific case, not just the subset required
      // by the ECMAScript specification.
      uc32 result = current();
      Advance();
      return result;
    }
  }
  return 0;
}


CharacterRange RegExpParser::ParseClassAtom(uc16* char_class) {
  ASSERT_EQ(0, *char_class);
  uc32 first = current();
  if (first == '\\') {
    switch (Next()) {
      case 'w': case 'W': case 'd': case 'D': case 's': case 'S': {
        *char_class = Next();
        Advance(2);
        return CharacterRange::Singleton(0);  // Return dummy value.
      }
      case kEndMarker:
        return ReportError(CStrVector("\\ at end of pattern"));
      default:
        uc32 c = ParseClassCharacterEscape(CHECK_FAILED);
        return CharacterRange::Singleton(c);
    }
  } else {
    Advance();
    return CharacterRange::Singleton(first);
  }
}


static const uc16 kNoCharClass = 0;

// Adds range or pre-defined character class to character ranges.
// If char_class is not kInvalidClass, it's interpreted as a class
// escape (i.e., 's' means whitespace, from '\s').
static inline void AddRangeOrEscape(ZoneList<CharacterRange>* ranges,
                                    uc16 char_class,
                                    CharacterRange range) {
  if (char_class != kNoCharClass) {
    CharacterRange::AddClassEscape(char_class, ranges);
  } else {
    ranges->Add(range);
  }
}


RegExpTree* RegExpParser::ParseCharacterClass() {
  static const char* kUnterminated = "Unterminated character class";
  static const char* kRangeOutOfOrder = "Range out of order in character class";

  ASSERT_EQ(current(), '[');
  Advance();
  bool is_negated = false;
  if (current() == '^') {
    is_negated = true;
    Advance();
  }
  ZoneList<CharacterRange>* ranges = new(zone()) ZoneList<CharacterRange>(2);
  while (has_more() && current() != ']') {
    uc16 char_class = kNoCharClass;
    CharacterRange first = ParseClassAtom(&char_class CHECK_FAILED);
    if (current() == '-') {
      Advance();
      if (current() == kEndMarker) {
        // If we reach the end we break out of the loop and let the
        // following code report an error.
        break;
      } else if (current() == ']') {
        AddRangeOrEscape(ranges, char_class, first);
        ranges->Add(CharacterRange::Singleton('-'));
        break;
      }
      uc16 char_class_2 = kNoCharClass;
      CharacterRange next = ParseClassAtom(&char_class_2 CHECK_FAILED);
      if (char_class != kNoCharClass || char_class_2 != kNoCharClass) {
        // Either end is an escaped character class. Treat the '-' verbatim.
        AddRangeOrEscape(ranges, char_class, first);
        ranges->Add(CharacterRange::Singleton('-'));
        AddRangeOrEscape(ranges, char_class_2, next);
        continue;
      }
      if (first.from() > next.to()) {
        return ReportError(CStrVector(kRangeOutOfOrder) CHECK_FAILED);
      }
      ranges->Add(CharacterRange::Range(first.from(), next.to()));
    } else {
      AddRangeOrEscape(ranges, char_class, first);
    }
  }
  if (!has_more()) {
    return ReportError(CStrVector(kUnterminated) CHECK_FAILED);
  }
  Advance();
  if (ranges->length() == 0) {
    ranges->Add(CharacterRange::Everything());
    is_negated = !is_negated;
  }
  return new(zone()) RegExpCharacterClass(ranges, is_negated);
}


// ----------------------------------------------------------------------------
// The Parser interface.

ParserMessage::~ParserMessage() {
  for (int i = 0; i < args().length(); i++)
    DeleteArray(args()[i]);
  DeleteArray(args().start());
}


ScriptDataImpl::~ScriptDataImpl() {
  if (owns_store_) store_.Dispose();
}


int ScriptDataImpl::Length() {
  return store_.length() * sizeof(unsigned);
}


const char* ScriptDataImpl::Data() {
  return reinterpret_cast<const char*>(store_.start());
}


bool ScriptDataImpl::HasError() {
  return has_error();
}


void ScriptDataImpl::Initialize() {
  // Prepares state for use.
  if (store_.length() >= PreparseDataConstants::kHeaderSize) {
    function_index_ = PreparseDataConstants::kHeaderSize;
    int symbol_data_offset = PreparseDataConstants::kHeaderSize
        + store_[PreparseDataConstants::kFunctionsSizeOffset];
    if (store_.length() > symbol_data_offset) {
      symbol_data_ = reinterpret_cast<byte*>(&store_[symbol_data_offset]);
    } else {
      // Partial preparse causes no symbol information.
      symbol_data_ = reinterpret_cast<byte*>(&store_[0] + store_.length());
    }
    symbol_data_end_ = reinterpret_cast<byte*>(&store_[0] + store_.length());
  }
}


int ScriptDataImpl::ReadNumber(byte** source) {
  // Reads a number from symbol_data_ in base 128. The most significant
  // bit marks that there are more digits.
  // If the first byte is 0x80 (kNumberTerminator), it would normally
  // represent a leading zero. Since that is useless, and therefore won't
  // appear as the first digit of any actual value, it is used to
  // mark the end of the input stream.
  byte* data = *source;
  if (data >= symbol_data_end_) return -1;
  byte input = *data;
  if (input == PreparseDataConstants::kNumberTerminator) {
    // End of stream marker.
    return -1;
  }
  int result = input & 0x7f;
  data++;
  while ((input & 0x80u) != 0) {
    if (data >= symbol_data_end_) return -1;
    input = *data;
    result = (result << 7) | (input & 0x7f);
    data++;
  }
  *source = data;
  return result;
}


// Create a Scanner for the preparser to use as input, and preparse the source.
static ScriptDataImpl* DoPreParse(UC16CharacterStream* source,
                                  int flags,
                                  ParserRecorder* recorder) {
  Isolate* isolate = Isolate::Current();
  HistogramTimerScope timer(isolate->counters()->pre_parse());
  Scanner scanner(isolate->unicode_cache());
  scanner.SetHarmonyScoping(FLAG_harmony_scoping);
  scanner.Initialize(source);
  intptr_t stack_limit = isolate->stack_guard()->real_climit();
  preparser::PreParser::PreParseResult result =
      preparser::PreParser::PreParseProgram(&scanner,
                                            recorder,
                                            flags,
                                            stack_limit);
  if (result == preparser::PreParser::kPreParseStackOverflow) {
    isolate->StackOverflow();
    return NULL;
  }

  // Extract the accumulated data from the recorder as a single
  // contiguous vector that we are responsible for disposing.
  Vector<unsigned> store = recorder->ExtractData();
  return new ScriptDataImpl(store);
}


// Preparse, but only collect data that is immediately useful,
// even if the preparser data is only used once.
ScriptDataImpl* ParserApi::PartialPreParse(Handle<String> source,
                                           v8::Extension* extension,
                                           int flags) {
  bool allow_lazy = FLAG_lazy && (extension == NULL);
  if (!allow_lazy) {
    // Partial preparsing is only about lazily compiled functions.
    // If we don't allow lazy compilation, the log data will be empty.
    return NULL;
  }
  flags |= kAllowLazy;
  PartialParserRecorder recorder;
  int source_length = source->length();
  if (source->IsExternalTwoByteString()) {
    ExternalTwoByteStringUC16CharacterStream stream(
        Handle<ExternalTwoByteString>::cast(source), 0, source_length);
    return DoPreParse(&stream, flags, &recorder);
  } else {
    GenericStringUC16CharacterStream stream(source, 0, source_length);
    return DoPreParse(&stream, flags, &recorder);
  }
}


ScriptDataImpl* ParserApi::PreParse(UC16CharacterStream* source,
                                    v8::Extension* extension,
                                    int flags) {
  Handle<Script> no_script;
  if (FLAG_lazy && (extension == NULL)) {
    flags |= kAllowLazy;
  }
  CompleteParserRecorder recorder;
  return DoPreParse(source, flags, &recorder);
}


bool RegExpParser::ParseRegExp(FlatStringReader* input,
                               bool multiline,
                               RegExpCompileData* result) {
  ASSERT(result != NULL);
  RegExpParser parser(input, &result->error, multiline);
  RegExpTree* tree = parser.ParsePattern();
  if (parser.failed()) {
    ASSERT(tree == NULL);
    ASSERT(!result->error.is_null());
  } else {
    ASSERT(tree != NULL);
    ASSERT(result->error.is_null());
    result->tree = tree;
    int capture_count = parser.captures_started();
    result->simple = tree->IsAtom() && parser.simple() && capture_count == 0;
    result->contains_anchor = parser.contains_anchor();
    result->capture_count = capture_count;
  }
  return !parser.failed();
}


bool ParserApi::Parse(CompilationInfo* info, int parsing_flags) {
  ASSERT(info->function() == NULL);
  FunctionLiteral* result = NULL;
  Handle<Script> script = info->script();
  ASSERT((parsing_flags & kLanguageModeMask) == CLASSIC_MODE);
  if (!info->is_native() && FLAG_harmony_scoping) {
    // Harmony scoping is requested.
    parsing_flags |= EXTENDED_MODE;
  }
  if (FLAG_allow_natives_syntax || info->is_native()) {
    // We requre %identifier(..) syntax.
    parsing_flags |= kAllowNativesSyntax;
  }
  if (info->is_lazy()) {
    ASSERT(!info->is_eval());
    Parser parser(script, parsing_flags, NULL, NULL);
    result = parser.ParseLazy(info);
  } else {
    ScriptDataImpl* pre_data = info->pre_parse_data();
    Parser parser(script, parsing_flags, info->extension(), pre_data);
    if (pre_data != NULL && pre_data->has_error()) {
      Scanner::Location loc = pre_data->MessageLocation();
      const char* message = pre_data->BuildMessage();
      Vector<const char*> args = pre_data->BuildArgs();
      parser.ReportMessageAt(loc, message, args);
      DeleteArray(message);
      for (int i = 0; i < args.length(); i++) {
        DeleteArray(args[i]);
      }
      DeleteArray(args.start());
      ASSERT(info->isolate()->has_pending_exception());
    } else {
      result = parser.ParseProgram(info);
    }
  }
  info->SetFunction(result);
  return (result != NULL);
}

} }  // namespace v8::internal