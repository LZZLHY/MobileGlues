// MobileGlues - gl/glsl/uniform_initializer_core.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_GLSL_UNIFORM_INITIALIZER_CORE_H
#define MOBILEGLUES_GLSL_UNIFORM_INITIALIZER_CORE_H

#include <cstddef>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Uniform declarations carrying an initializer
// ---------------------------------------------------------------------------
// Desktop GLSL lets a uniform be declared with an initial value; GLSL ES does
// not ("uniform variables ... cannot have initializers", ESSL 3.00/3.20 4.3.5).
// A shader can therefore leave SPIRV-Cross as valid desktop GLSL and still be
// rejected by an ES front end, so the translator strips the initializer and
// keeps the declaration.
//
// That compatibility duty is not in question here. How it used to be carried
// out was: the previous implementation searched for the *substring* `uniform`
// at any character position, and then decided a declaration had an initializer
// if any `=` appeared anywhere before the next `;`. Both halves are wrong in
// the same direction -- they accept text that is not a uniform declaration --
// and together they destroyed shaders that never contained a uniform
// initializer at all.
//
// The observed failure, from the ESSL actually handed to the driver: SPIRV-Cross
// names its UBO instances `_uniform_instance_<n>_<m>`. In
//
//     if (_uniform_instance_00_01.UseRgss == 1)
//     {
//         highp vec2 param = ...;
//
// the substring scan starts inside that identifier, reads `_instance_00_01` as
// a type name, accepts the `==` of the condition as an initializer, and finds
// its "declaration end" at the semicolon of the *following* statement. The
// rewrite then replaced everything from the middle of the identifier to that
// semicolon, producing
//
//     if (_uniform _instance_00_01 ;
//         highp vec2 param_1 = ...;
//
// which is four driver errors per shader (`_uniform`, `param`, `param_2`
// undeclared, then a stray `else`) and, for Minecraft 26.3 snapshot 9, twelve
// unusable terrain fragment programs and an immediate exit.
//
// So the rule for this pass is fail-closed: rewrite only what it can prove is a
// global uniform declaration with exactly one initializer, and leave every other
// byte alone.
//
//   1. `uniform` counts only as a whole identifier token. The lexer consumes
//      complete identifier runs, so a match inside `_uniform_instance`,
//      `nonuniform` or `uniformity` is not merely rejected -- it is never
//      offered.
//   2. Only global scope. A `uniform` token inside braces, parentheses or
//      brackets is not a declaration this pass may touch. This is a second,
//      independent guard against the failure above, which occurred inside a
//      function body.
//   3. The declarator must be a single simple variable: identifier tokens
//      (qualifiers/type, then the name) followed by optional balanced array
//      suffixes. Nothing else.
//   4. The initializer must be the very next token after the declarator, it
//      must be a lone `=`, and it is found by looking at that one position --
//      never by searching the text. `==`, `!=`, `>=` and an assignment in a
//      later statement are all simply not at that position.
//   5. The terminating `;` is found with the same lexer, at bracket depth zero,
//      skipping comments. Unbalanced nesting, an unterminated comment, a
//      preprocessor directive inside the declaration, a further declarator after
//      a comma, or a missing `;` all mean "not understood", and nothing is
//      changed. The comma is not a nicety: without it this scan runs past the
//      `,` in `uniform float a = 1.0, b = 2.0;` and the rewrite deletes `b` --
//      the same fail-open shape as the defect above, one layer down.
//   6. Comments and whitespace are state in the lexer, not text to match. A
//      `uniform` inside a comment is a comment.
//
// Diagnosis is allowed to be approximate where rewriting may not be: when a
// declaration is left alone, a separate lexical scan reports whether it looks
// like it carried an initializer, so a driver rejection can be traced back here
// instead of being invisible. That scan never decides whether to rewrite.
//
// This is a lexical defect in a GLSL-to-ESSL pass and a specification-level ES
// restriction. It is deliberately not conditioned on any platform, driver,
// vendor, application or shader name, and is an upstream candidate.
//
// Kept in its own header, with no dependency beyond the standard library, so the
// whole thing can be compiled and exercised off-device: the previous
// implementation was plausible-looking code whose defect only ever appeared as
// a driver error message on a phone.
namespace mg::glsl {

// Why each of these is a separate counter rather than one "not rewritten"
// number: the correct reaction differs. A declaration that never had an
// initializer is the overwhelmingly common case and is not worth a word, while
// one this pass refused to touch *with* an initializer present is the case a
// later driver rejection has to be traceable to. Merging causes into a single
// value is what makes a defect unfindable once it is only reproducible on a
// device.
struct UniformInitializerStats {
    // `uniform` identifier tokens seen, at any scope.
    std::size_t seen = 0;
    // Declarations rewritten: `uniform T n = expr;` -> `uniform T n;`
    std::size_t stripped = 0;
    // Global declarations already in the form ES requires.
    std::size_t no_initializer = 0;
    // `uniform` token inside braces/parentheses/brackets.
    std::size_t not_at_global_scope = 0;

    // Fail-closed outcomes. Each one is a shape this pass does not claim to
    // understand, and each leaves the source bytes untouched.
    std::size_t unhandled_declarator = 0;
    std::size_t unhandled_interface_block = 0;
    std::size_t unhandled_multiple_declarators = 0;
    std::size_t unhandled_preprocessor = 0;
    std::size_t unhandled_unterminated = 0;

    // Subset of the unhandled_* total that a diagnostic-only scan believes
    // carries an initializer. Nonzero here means the driver is about to see a
    // declaration ES does not allow, and this pass knows it.
    std::size_t unhandled_with_initializer = 0;

    std::size_t unhandled() const {
        return unhandled_declarator + unhandled_interface_block + unhandled_multiple_declarators +
               unhandled_preprocessor + unhandled_unterminated;
    }
};

struct UniformInitializerRewrite {
    std::string code;
    UniformInitializerStats stats;
};

// One outcome per `uniform` token. Exposed so a host test can assert *why* a
// declaration was left alone: "did not crash" and "declined for the right
// reason" are different claims, and only the second one survives a refactor.
enum class UniformDeclOutcome {
    StrippedInitializer,
    NoInitializer,
    NotAtGlobalScope,
    UnhandledDeclarator,
    UnhandledInterfaceBlock,
    UnhandledMultipleDeclarators,
    UnhandledPreprocessor,
    UnhandledUnterminated,
};

struct UniformDeclSpan {
    // One past the last byte of the declarator (name, or its array suffix).
    std::size_t declarator_end = 0;
    // Index of the initializer `=`. Only meaningful for StrippedInitializer.
    std::size_t initializer_eq = 0;
    // Index of the `;` that ends the declaration. StrippedInitializer only.
    std::size_t semicolon = 0;
    // A comment sits between the declarator and the `=`, so the bytes in that
    // gap are kept rather than collapsed.
    bool comment_before_initializer = false;
};

namespace detail {

// Deliberately not <cctype>: those are locale-sensitive and take an int whose
// domain is not char. A GLSL identifier is defined by the grammar, not by the
// host locale.
inline bool isIdentifierChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

inline bool isIdentifierStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

inline bool isSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

struct TriviaSpan {
    // False only for an unterminated block comment: from there to the end of
    // the file is comment, and nothing after it can be parsed.
    bool ok = true;
    bool saw_comment = false;
};

// Advances past whitespace and comments. A `\` at the end of a line inside a
// `//` comment continues that comment, per the line splicing GLSL inherits from
// C -- getting this wrong would make the pass read the next line as code.
inline TriviaSpan skipTrivia(std::string_view s, std::size_t& p) {
    TriviaSpan span;
    for (;;) {
        while (p < s.size() && isSpaceChar(s[p])) ++p;
        if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '/') {
            span.saw_comment = true;
            p += 2;
            while (p < s.size() && s[p] != '\n') {
                if (s[p] == '\\') {
                    if (p + 1 < s.size() && s[p + 1] == '\n') {
                        p += 2;
                        continue;
                    }
                    if (p + 2 < s.size() && s[p + 1] == '\r' && s[p + 2] == '\n') {
                        p += 3;
                        continue;
                    }
                }
                ++p;
            }
            continue;
        }
        if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '*') {
            span.saw_comment = true;
            const std::size_t close = s.find("*/", p + 2);
            if (close == std::string_view::npos) {
                p = s.size();
                span.ok = false;
                return span;
            }
            p = close + 2;
            continue;
        }
        return span;
    }
}

// Consumes one balanced `(...)`, `[...]` or `{...}` group starting at s[p].
// A `;` or a preprocessor directive inside the group means the text is not the
// shape this pass understands, so it reports failure rather than guessing where
// the group ends.
inline bool consumeBalancedGroup(std::string_view s, std::size_t& p) {
    if (p >= s.size()) return false;
    const char open = s[p];
    char close = '\0';
    if (open == '(') close = ')';
    else if (open == '[') close = ']';
    else if (open == '{') close = '}';
    else return false;

    std::size_t depth = 0;
    while (p < s.size()) {
        if (!skipTrivia(s, p).ok) return false;
        if (p >= s.size()) return false;
        const char c = s[p];
        if (c == open) {
            ++depth;
            ++p;
            continue;
        }
        if (c == close) {
            if (depth == 0) return false;
            --depth;
            ++p;
            if (depth == 0) return true;
            continue;
        }
        if (c == ';' || c == '#') return false;
        ++p;
    }
    return false;
}

// Four values, not two. "A directive sits inside this declaration", "there is
// another declarator after the initializer" and "the text simply ran out" all
// lead to the same refusal but are different facts, and folding several causes
// into one code is how a defect stops being findable once the only reproducer is
// a device.
enum class DeclEndScan {
    Found,
    Comma,
    Preprocessor,
    Unterminated,
};

// Advances p to the `;` that ends the declaration, at nesting depth zero.
//
// The comma case is not a nicety. Without it this scan walks straight past the
// `,` in `uniform float a = 1.0, b = 2.0;`, calls the final `;` the end of the
// declaration, and the caller then deletes `b` along with the initializer -- the
// same fail-open shape as the defect this whole pass exists to remove, just one
// layer down. The host test caught it; the reasoning did not.
inline DeclEndScan findDeclarationEnd(std::string_view s, std::size_t& p) {
    while (p < s.size()) {
        if (!skipTrivia(s, p).ok) return DeclEndScan::Unterminated;
        if (p >= s.size()) break;
        const char c = s[p];
        if (c == ';') return DeclEndScan::Found;
        if (c == ',') return DeclEndScan::Comma;
        // A directive inside a declaration means the text the driver sees is
        // not the text this pass is reading.
        if (c == '#') return DeclEndScan::Preprocessor;
        if (c == '(' || c == '[' || c == '{') {
            if (!consumeBalancedGroup(s, p)) return DeclEndScan::Unterminated;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') return DeclEndScan::Unterminated;
        ++p;
    }
    return DeclEndScan::Unterminated;
}

// Diagnostic only: does the declaration starting at `from` appear to carry an
// initializer? Compound operators are stepped over so `==`, `!=`, `>=` and
// friends do not count. This never gates a rewrite -- it exists so that a
// declaration this pass declined can still be named in a log when the driver
// rejects it.
inline bool declarationLooksInitialised(std::string_view s, std::size_t from) {
    std::size_t p = from;
    while (p < s.size()) {
        if (!skipTrivia(s, p).ok) return false;
        if (p >= s.size()) return false;
        const char c = s[p];
        if (c == ';' || c == '#') return false;
        if (c == '(' || c == '[' || c == '{') {
            if (!consumeBalancedGroup(s, p)) return false;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') return false;
        if (c == '=') {
            if (p + 1 < s.size() && s[p + 1] == '=') {
                p += 2;
                continue;
            }
            return true;
        }
        if ((c == '!' || c == '<' || c == '>' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
             c == '&' || c == '|' || c == '^') &&
            p + 1 < s.size() && s[p + 1] == '=') {
            p += 2;
            continue;
        }
        ++p;
    }
    return false;
}

} // namespace detail

// Classifies the declaration that follows a `uniform` keyword token.
// `after_keyword` is the index one past that token; the caller has already
// established that the token stood alone and at global scope.
inline UniformDeclOutcome classifyGlobalUniformDecl(std::string_view s, std::size_t after_keyword,
                                                   UniformDeclSpan& span) {
    span = UniformDeclSpan{};

    std::size_t p = after_keyword;
    detail::TriviaSpan trivia = detail::skipTrivia(s, p);
    if (!trivia.ok) return UniformDeclOutcome::UnhandledUnterminated;
    if (p >= s.size()) return UniformDeclOutcome::UnhandledUnterminated;
    if (s[p] == '#') return UniformDeclOutcome::UnhandledPreprocessor;

    // Qualifier/type tokens followed by the variable name. Two is the minimum
    // that can be a declaration (`<type> <name>`); anything shorter is a shape
    // this pass will not name.
    std::size_t identifier_tokens = 0;
    std::size_t declarator_end = 0;
    while (p < s.size() && detail::isIdentifierStart(s[p])) {
        while (p < s.size() && detail::isIdentifierChar(s[p])) ++p;
        declarator_end = p;
        ++identifier_tokens;
        trivia = detail::skipTrivia(s, p);
        if (!trivia.ok) return UniformDeclOutcome::UnhandledUnterminated;
    }
    if (identifier_tokens < 2) {
        if (p < s.size() && s[p] == '{') return UniformDeclOutcome::UnhandledInterfaceBlock;
        return UniformDeclOutcome::UnhandledDeclarator;
    }

    // Optional array suffixes. Balanced brackets only; the contents are not
    // interpreted, because they do not need to be.
    while (p < s.size() && s[p] == '[') {
        if (!detail::consumeBalancedGroup(s, p)) return UniformDeclOutcome::UnhandledUnterminated;
        declarator_end = p;
        trivia = detail::skipTrivia(s, p);
        if (!trivia.ok) return UniformDeclOutcome::UnhandledUnterminated;
    }

    span.declarator_end = declarator_end;
    span.comment_before_initializer = trivia.saw_comment;

    if (p >= s.size()) return UniformDeclOutcome::UnhandledUnterminated;

    switch (s[p]) {
    case ';':
        return UniformDeclOutcome::NoInitializer;
    case '{':
        // `uniform Block { ... } inst;`: an interface block, whose members are
        // not this pass's business.
        return UniformDeclOutcome::UnhandledInterfaceBlock;
    case ',':
        return UniformDeclOutcome::UnhandledMultipleDeclarators;
    case '#':
        return UniformDeclOutcome::UnhandledPreprocessor;
    case '=':
        break;
    default:
        return UniformDeclOutcome::UnhandledDeclarator;
    }

    // A lone `=`. `==` at this position cannot be an initializer, and reaching
    // it would mean the declarator parse above already went wrong.
    if (p + 1 < s.size() && s[p + 1] == '=') return UniformDeclOutcome::UnhandledDeclarator;

    span.initializer_eq = p;
    ++p;
    switch (detail::findDeclarationEnd(s, p)) {
    case detail::DeclEndScan::Found:
        break;
    case detail::DeclEndScan::Comma:
        return UniformDeclOutcome::UnhandledMultipleDeclarators;
    case detail::DeclEndScan::Preprocessor:
        return UniformDeclOutcome::UnhandledPreprocessor;
    case detail::DeclEndScan::Unterminated:
        return UniformDeclOutcome::UnhandledUnterminated;
    }
    span.semicolon = p;
    return UniformDeclOutcome::StrippedInitializer;
}

// Removes the initializer from every global uniform declaration this pass can
// prove is one, and copies every other byte through unchanged. With nothing to
// strip the result is byte-for-byte identical to the input.
inline UniformInitializerRewrite stripGlobalUniformInitializers(std::string_view source) {
    UniformInitializerRewrite out;
    out.code.reserve(source.size());

    const std::size_t n = source.size();
    std::size_t i = 0;
    // Start of the run of bytes not yet copied to the output.
    std::size_t chunk = 0;
    std::size_t brace_depth = 0;
    std::size_t paren_depth = 0;
    std::size_t bracket_depth = 0;
    // Only whitespace and comments seen on this line so far, so a `#` here
    // opens a preprocessor directive. Comments do not clear it: in C, and so in
    // GLSL, a directive may be preceded by them.
    bool at_line_start = true;

    while (i < n) {
        const char c = source[i];

        if (c == '/' && i + 1 < n && (source[i + 1] == '/' || source[i + 1] == '*')) {
            const std::size_t before = i;
            if (!detail::skipTrivia(source, i).ok) break;
            // Whether a following `#` opens a directive depends on having
            // crossed a line boundary, and a comment (or the whitespace after
            // it) is free to cross one. Without this, `int a; /* c */` on one
            // line would make the next line's `#define` read as code.
            const std::size_t newline = source.find('\n', before);
            if (newline != std::string_view::npos && newline < i) at_line_start = true;
            continue;
        }
        if (c == '#' && at_line_start) {
            // Skip the whole directive, honouring line splicing. Its text is
            // not the shader the driver compiles, and a `uniform` inside a
            // macro body is not a declaration this pass may rewrite.
            ++i;
            while (i < n && source[i] != '\n') {
                if (source[i] == '\\') {
                    if (i + 1 < n && source[i + 1] == '\n') {
                        i += 2;
                        continue;
                    }
                    if (i + 2 < n && source[i + 1] == '\r' && source[i + 2] == '\n') {
                        i += 3;
                        continue;
                    }
                }
                ++i;
            }
            continue;
        }
        if (c == '\n') {
            at_line_start = true;
            ++i;
            continue;
        }
        if (detail::isSpaceChar(c)) {
            ++i;
            continue;
        }

        at_line_start = false;

        if (c == '{') {
            ++brace_depth;
            ++i;
            continue;
        }
        if (c == '}') {
            if (brace_depth > 0) --brace_depth;
            ++i;
            continue;
        }
        if (c == '(') {
            ++paren_depth;
            ++i;
            continue;
        }
        if (c == ')') {
            if (paren_depth > 0) --paren_depth;
            ++i;
            continue;
        }
        if (c == '[') {
            ++bracket_depth;
            ++i;
            continue;
        }
        if (c == ']') {
            if (bracket_depth > 0) --bracket_depth;
            ++i;
            continue;
        }

        if (!detail::isIdentifierChar(c)) {
            ++i;
            continue;
        }

        // Consume the entire identifier or number run. This is the whole reason
        // `_uniform_instance_00_01` can no longer be mistaken for the keyword:
        // the substring is never a token in the first place.
        const std::size_t token_start = i;
        while (i < n && detail::isIdentifierChar(source[i])) ++i;
        if (source.compare(token_start, i - token_start, "uniform") != 0) continue;

        ++out.stats.seen;
        if (brace_depth != 0 || paren_depth != 0 || bracket_depth != 0) {
            ++out.stats.not_at_global_scope;
            continue;
        }

        UniformDeclSpan span;
        const UniformDeclOutcome outcome = classifyGlobalUniformDecl(source, i, span);
        switch (outcome) {
        case UniformDeclOutcome::StrippedInitializer: {
            // Keep the declarator byte for byte. The whitespace before `=` is
            // dropped so the common case reads as ES wants it; if a comment is
            // sitting there, those bytes are kept instead of discarded.
            const std::size_t cut_end = span.comment_before_initializer ? span.initializer_eq : span.declarator_end;
            out.code.append(source.substr(chunk, cut_end - chunk));
            out.code.push_back(';');
            ++out.stats.stripped;
            i = span.semicolon + 1;
            chunk = i;
            break;
        }
        case UniformDeclOutcome::NoInitializer:
            ++out.stats.no_initializer;
            break;
        case UniformDeclOutcome::NotAtGlobalScope:
            // Decided by the caller above; unreachable here, and counted there.
            ++out.stats.not_at_global_scope;
            break;
        case UniformDeclOutcome::UnhandledDeclarator:
            ++out.stats.unhandled_declarator;
            break;
        case UniformDeclOutcome::UnhandledInterfaceBlock:
            ++out.stats.unhandled_interface_block;
            break;
        case UniformDeclOutcome::UnhandledMultipleDeclarators:
            ++out.stats.unhandled_multiple_declarators;
            break;
        case UniformDeclOutcome::UnhandledPreprocessor:
            ++out.stats.unhandled_preprocessor;
            break;
        case UniformDeclOutcome::UnhandledUnterminated:
            ++out.stats.unhandled_unterminated;
            break;
        }

        if (outcome != UniformDeclOutcome::StrippedInitializer && outcome != UniformDeclOutcome::NoInitializer) {
            if (detail::declarationLooksInitialised(source, i)) {
                ++out.stats.unhandled_with_initializer;
            }
        }
    }

    if (chunk < n) {
        out.code.append(source.substr(chunk, n - chunk));
    }
    return out;
}

} // namespace mg::glsl

#endif // MOBILEGLUES_GLSL_UNIFORM_INITIALIZER_CORE_H
