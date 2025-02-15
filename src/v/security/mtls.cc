/*
 * Copyright 2022 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "security/mtls.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>

#include <regex>
#include <stdexcept>

namespace security::tls {

namespace detail {

static constexpr const char* const rule_pattern{
  R"((DEFAULT)|RULE:((\\.|[^\\/])*)\/((\\.|[^\\/])*)\/([LU]?).*?|(.*?))"};
static constexpr const char* const rule_pattern_splitter{
  R"(\s*((DEFAULT)|RULE:((\\.|[^\\/])*)\/((\\.|[^\\/])*)\/([LU]?).*?|(.*?))\s*([,\n]\s*|$))"};

std::regex make_regex(std::string_view sv) {
    return std::regex{
      sv.begin(),
      sv.length(),
      std::regex_constants::ECMAScript | std::regex_constants::optimize};
}

bool regex_search(
  std::string_view msg, std::cmatch& match, std::regex const& regex) {
    return std::regex_search(
      msg.begin(),
      msg.end(),
      match,
      regex,
      std::regex_constants::match_default);
}

bool regex_match(
  std::string_view msg, std::cmatch& match, std::regex const& regex) {
    return std::regex_match(
      msg.begin(),
      msg.end(),
      match,
      regex,
      std::regex_constants::match_default);
}

constexpr std::string_view trim(std::string_view s) {
    constexpr auto ws = " \t\n\r\f\v";
    s.remove_prefix(std::min(s.find_first_not_of(ws), s.size()));
    s.remove_suffix(std::min(s.size() - s.find_last_not_of(ws) - 1, s.size()));
    return s;
}

constexpr std::optional<std::string_view> make_sv(const std::csub_match& sm) {
    return sm.matched
             ? std::string_view{sm.first, static_cast<size_t>(sm.length())}
             : std::optional<std::string_view>{std::nullopt};
}

std::vector<rule>
parse_rules(std::optional<std::vector<ss::sstring>> unparsed_rules) {
    static const std::regex rule_splitter = make_regex(rule_pattern_splitter);
    static const std::regex rule_parser = make_regex(rule_pattern);

    std::string rules
      = unparsed_rules.has_value() ? fmt::format(
          "{}", fmt::join(unparsed_rules->begin(), unparsed_rules->end(), ","))
                                   : "DEFAULT";

    std::vector<rule> result;
    std::cmatch rules_match;
    while (!rules.empty() && regex_search(rules, rules_match, rule_splitter)) {
        const auto& rule{rules_match[1]};

        std::cmatch components_match;
        if (!regex_search(*make_sv(rule), components_match, rule_parser)) {
            throw std::runtime_error("Invalid rule: " + rule.str());
        }
        if (components_match.prefix().matched) {
            throw std::runtime_error(
              "Invalid rule - prefix: " + components_match.prefix().str());
        }
        if (components_match.suffix().matched) {
            throw std::runtime_error(
              "Invalid rule - suffix: " + components_match.suffix().str());
        }

        if (components_match[1].matched) {
            result.emplace_back();
        } else if (components_match[2].matched) {
            const auto adjust_case = make_sv(components_match[6]);
            result.emplace_back(
              *make_sv(components_match[2]),
              make_sv(components_match[4]),
              rule::make_lower{adjust_case == "L"},
              rule::make_upper{adjust_case == "U"});
        }
        rules = make_sv(rules_match.suffix()).value_or("");
    }
    return result;
}

} // namespace detail

rule::rule(
  std::string_view pattern,
  std::optional<std::string_view> replacement,
  make_lower to_lower,
  make_upper to_upper)
  : _regex{detail::make_regex(pattern)}
  , _pattern{pattern}
  , _replacement{replacement}
  , _is_default{false}
  , _to_lower{to_lower}
  , _to_upper{to_upper} {}

std::optional<ss::sstring> rule::apply(std::string_view dn) const {
    if (_is_default) {
        return ss::sstring{dn};
    }

    std::cmatch match;
    if (!std::regex_match(dn.cbegin(), dn.cend(), match, _regex)) {
        return {};
    }

    std::string result;
    std::regex_replace(
      std::back_inserter(result),
      dn.begin(),
      dn.end(),
      _regex,
      _replacement.value_or("").c_str());

    if (_to_lower) {
        boost::algorithm::to_lower(result, std::locale::classic());
    } else if (_to_upper) {
        boost::algorithm::to_upper(result, std::locale::classic());
    }
    return result;
}

std::optional<ss::sstring>
validate_rules(const std::optional<std::vector<ss::sstring>>& r) noexcept {
    try {
        security::tls::detail::parse_rules(r);
    } catch (const std::exception& e) {
        return e.what();
    }
    return std::nullopt;
}

std::ostream& operator<<(std::ostream& os, const rule& r) {
    fmt::print(os, "{}", r);
    return os;
}

std::ostream& operator<<(std::ostream& os, const principal_mapper& p) {
    fmt::print(os, "{}", p);
    return os;
}

principal_mapper::principal_mapper(
  config::binding<std::optional<std::vector<ss::sstring>>> cb)
  : _binding(std::move(cb))
  , _rules{detail::parse_rules(_binding())} {
    _binding.watch([this]() { _rules = detail::parse_rules(_binding()); });
}

std::optional<ss::sstring> principal_mapper::apply(std::string_view sv) const {
    for (const auto& r : _rules) {
        if (auto p = r.apply(sv); p.has_value()) {
            return {std::move(p).value()};
        }
    }
    return std::nullopt;
}

} // namespace security::tls

// explicit instantiations so as to avoid bringing in <fmt/ranges.h> in the
// header, whch breaks compilation in another part of the codebase.
template<>
typename fmt::basic_format_context<fmt::appender, char>::iterator
fmt::formatter<security::tls::rule, char, void>::format<
  fmt::basic_format_context<fmt::appender, char>>(
  security::tls::rule const& r,
  fmt::basic_format_context<fmt::appender, char>& ctx) const {
    if (r._is_default) {
        return fmt::format_to(ctx.out(), "DEFAULT");
    }
    fmt::format_to(ctx.out(), "RULE:");
    if (r._pattern.has_value()) {
        fmt::format_to(ctx.out(), "{}", *r._pattern);
    }
    if (r._replacement.has_value()) {
        fmt::format_to(ctx.out(), "/{}", *r._replacement);
    }
    if (r._to_lower) {
        fmt::format_to(ctx.out(), "/L");
    } else if (r._to_upper) {
        fmt::format_to(ctx.out(), "/U");
    }
    return ctx.out();
}

template<>
typename fmt::basic_format_context<fmt::appender, char>::iterator
fmt::formatter<security::tls::principal_mapper, char, void>::format<
  fmt::basic_format_context<fmt::appender, char>>(
  security::tls::principal_mapper const& r,
  fmt::basic_format_context<fmt::appender, char>& ctx) const {
    return fmt::format_to(ctx.out(), "[{}]", fmt::join(r._rules, ", "));
}
