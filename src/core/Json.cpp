/*
 *  Copyright (C) 2026 Stephen Ferrell
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dt
{
namespace
{

class Parser
{
public:
  explicit Parser(const std::string& text) : m_text(text) {}

  bool Run(Json& out)
  {
    if (!Value(out, 0))
      return false;
    Space();
    return m_pos == m_text.size();
  }

private:
  void Space()
  {
    while (m_pos < m_text.size())
    {
      const char c = m_text[m_pos];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        break;
      ++m_pos;
    }
  }

  bool Literal(const char* word)
  {
    const std::string text(word);
    if (m_text.compare(m_pos, text.size(), text) != 0)
      return false;
    m_pos += text.size();
    return true;
  }

  bool Value(Json& out, int depth)
  {
    if (depth > 64)
      return false;
    Space();
    if (m_pos >= m_text.size())
      return false;

    switch (m_text[m_pos])
    {
      case '{':
        return ObjectValue(out, depth);
      case '[':
        return ArrayValue(out, depth);
      case '"':
      {
        std::string text;
        if (!StringValue(text))
          return false;
        out = Json(std::move(text));
        return true;
      }
      case 't':
        out = Json(true);
        return Literal("true");
      case 'f':
        out = Json(false);
        return Literal("false");
      case 'n':
        out = Json();
        return Literal("null");
      default:
        return NumberValue(out);
    }
  }

  bool ObjectValue(Json& out, int depth)
  {
    out = Json::Object();
    ++m_pos;
    Space();
    if (m_pos < m_text.size() && m_text[m_pos] == '}')
    {
      ++m_pos;
      return true;
    }
    while (true)
    {
      Space();
      std::string key;
      if (m_pos >= m_text.size() || m_text[m_pos] != '"' || !StringValue(key))
        return false;
      Space();
      if (m_pos >= m_text.size() || m_text[m_pos++] != ':')
        return false;
      Json value;
      if (!Value(value, depth + 1))
        return false;
      out.Set(key, std::move(value));
      Space();
      if (m_pos >= m_text.size())
        return false;
      const char c = m_text[m_pos++];
      if (c == '}')
        return true;
      if (c != ',')
        return false;
    }
  }

  bool ArrayValue(Json& out, int depth)
  {
    out = Json::Array();
    ++m_pos;
    Space();
    if (m_pos < m_text.size() && m_text[m_pos] == ']')
    {
      ++m_pos;
      return true;
    }
    while (true)
    {
      Json value;
      if (!Value(value, depth + 1))
        return false;
      out.Push(std::move(value));
      Space();
      if (m_pos >= m_text.size())
        return false;
      const char c = m_text[m_pos++];
      if (c == ']')
        return true;
      if (c != ',')
        return false;
    }
  }

  static void AppendUtf8(std::string& out, unsigned int code)
  {
    if (code < 0x80)
    {
      out.push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else if (code < 0x10000)
    {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else
    {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  bool Hex4(unsigned int& code)
  {
    if (m_pos + 4 > m_text.size())
      return false;
    code = 0;
    for (int i = 0; i < 4; ++i)
    {
      const char c = m_text[m_pos++];
      code <<= 4;
      if (c >= '0' && c <= '9')
        code |= static_cast<unsigned int>(c - '0');
      else if (c >= 'a' && c <= 'f')
        code |= static_cast<unsigned int>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        code |= static_cast<unsigned int>(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }

  bool StringValue(std::string& out)
  {
    ++m_pos;
    while (m_pos < m_text.size())
    {
      const char c = m_text[m_pos++];
      if (c == '"')
        return true;
      if (c != '\\')
      {
        out.push_back(c);
        continue;
      }
      if (m_pos >= m_text.size())
        return false;
      const char escape = m_text[m_pos++];
      switch (escape)
      {
        case 'n':
          out.push_back('\n');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'u':
        {
          unsigned int code;
          if (!Hex4(code))
            return false;
          if (code >= 0xD800 && code < 0xDC00 && m_text.compare(m_pos, 2, "\\u") == 0)
          {
            m_pos += 2;
            unsigned int low;
            if (!Hex4(low))
              return false;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(out, code);
          break;
        }
        default:
          out.push_back(escape);
          break;
      }
    }
    return false;
  }

  bool NumberValue(Json& out)
  {
    const char* start = m_text.c_str() + m_pos;
    char* end = nullptr;
    const double value = std::strtod(start, &end);
    if (end == start)
      return false;
    m_pos += static_cast<size_t>(end - start);
    out = Json(value);
    return true;
  }

  const std::string& m_text;
  size_t m_pos = 0;
};

void DumpString(const std::string& text, std::string& out)
{
  out.push_back('"');
  for (const char c : text)
  {
    switch (c)
    {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20)
        {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(c));
          out += buffer;
        }
        else
        {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

} // namespace

Json Json::Array()
{
  Json value;
  value.m_type = Type::Array;
  return value;
}

Json Json::Object()
{
  Json value;
  value.m_type = Type::Object;
  return value;
}

bool Json::Parse(const std::string& text, Json& out)
{
  Parser parser(text);
  return parser.Run(out);
}

std::string Json::Dump() const
{
  std::string out;
  switch (m_type)
  {
    case Type::Null:
      return "null";
    case Type::Bool:
      return m_bool ? "true" : "false";
    case Type::Number:
    {
      char buffer[40];
      if (m_number == std::floor(m_number) && std::fabs(m_number) < 9e15)
        std::snprintf(buffer, sizeof(buffer), "%.0f", m_number);
      else
        std::snprintf(buffer, sizeof(buffer), "%.17g", m_number);
      return buffer;
    }
    case Type::String:
      DumpString(m_string, out);
      return out;
    case Type::Array:
    {
      out.push_back('[');
      for (size_t i = 0; i < m_array.size(); ++i)
      {
        if (i)
          out.push_back(',');
        out += m_array[i].Dump();
      }
      out.push_back(']');
      return out;
    }
    case Type::Object:
    {
      out.push_back('{');
      for (size_t i = 0; i < m_object.size(); ++i)
      {
        if (i)
          out.push_back(',');
        DumpString(m_object[i].first, out);
        out.push_back(':');
        out += m_object[i].second.Dump();
      }
      out.push_back('}');
      return out;
    }
  }
  return out;
}

bool Json::AsBool(bool fallback) const
{
  if (m_type == Type::Bool)
    return m_bool;
  if (m_type == Type::Number)
    return m_number != 0;
  return fallback;
}

double Json::AsNumber(double fallback) const
{
  if (m_type == Type::Number)
    return m_number;
  if (m_type == Type::String)
    return std::atof(m_string.c_str());
  return fallback;
}

std::string Json::AsString(const std::string& fallback) const
{
  if (m_type == Type::String)
    return m_string;
  if (m_type == Type::Number)
    return Dump();
  return fallback;
}

const Json& Json::operator[](const char* key) const
{
  static const Json null;
  for (const auto& item : m_object)
    if (item.first == key)
      return item.second;
  return null;
}

bool Json::Has(const char* key) const
{
  for (const auto& item : m_object)
    if (item.first == key)
      return true;
  return false;
}

void Json::Set(const std::string& key, Json value)
{
  for (auto& item : m_object)
  {
    if (item.first == key)
    {
      item.second = std::move(value);
      return;
    }
  }
  m_object.emplace_back(key, std::move(value));
}

} // namespace dt
