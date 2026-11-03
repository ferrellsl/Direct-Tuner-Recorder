/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace dt
{

// A small JSON value: enough for the tuner's replies and our own state files.
class Json
{
public:
  enum class Type
  {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
  };

  Json() = default;
  Json(bool value) : m_type(Type::Bool), m_bool(value) {}
  Json(int value) : m_type(Type::Number), m_number(value) {}
  Json(unsigned int value) : m_type(Type::Number), m_number(value) {}
  Json(long long value) : m_type(Type::Number), m_number(static_cast<double>(value)) {}
  Json(double value) : m_type(Type::Number), m_number(value) {}
  Json(const char* value) : m_type(Type::String), m_string(value) {}
  Json(std::string value) : m_type(Type::String), m_string(std::move(value)) {}

  static Json Array();
  static Json Object();
  static bool Parse(const std::string& text, Json& out);

  std::string Dump() const;

  Type GetType() const { return m_type; }
  bool IsArray() const { return m_type == Type::Array; }
  bool IsObject() const { return m_type == Type::Object; }
  bool IsNull() const { return m_type == Type::Null; }

  bool AsBool(bool fallback = false) const;
  double AsNumber(double fallback = 0) const;
  int AsInt(int fallback = 0) const { return static_cast<int>(AsNumber(fallback)); }
  long long AsInt64(long long fallback = 0) const
  {
    return static_cast<long long>(AsNumber(static_cast<double>(fallback)));
  }
  std::string AsString(const std::string& fallback = {}) const;

  const std::vector<Json>& Items() const { return m_array; }
  const Json& operator[](const char* key) const;
  bool Has(const char* key) const;

  void Push(Json value) { m_array.push_back(std::move(value)); }
  void Set(const std::string& key, Json value);

private:
  Type m_type = Type::Null;
  bool m_bool = false;
  double m_number = 0;
  std::string m_string;
  std::vector<Json> m_array;
  std::vector<std::pair<std::string, Json>> m_object;
};

} // namespace dt
