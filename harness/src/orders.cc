// Copyright 2026 The wow-llm-bots Authors. Licensed under GPL-2.0.

#include "orders.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/strings/strcat.h"
#include "grammar/orders.wlb.h"
#include "grammar/wlb_runtime.h"

namespace harness {

std::optional<std::vector<Act>> ParseActs(
    std::string_view text,
    const std::vector<std::string>& names) {
  // The steps of an order are only a type inside `Order`: parse them as one.
  std::optional<Order> order = grammar::ParseOrder(
      base::StrCat({"always -> ", text}),
      grammar::rt::Env{.names = {{"order_names", names}}, .counts = {}});
  if (!order) {
    return std::nullopt;
  }
  return std::move(order->steps);
}

std::string OrderGrammar(const std::vector<std::string>& names) {
  // No names, no orders: that is how `order:` lines stay off until the module
  // can carry acts out (docs/specs/triggers.md, step (b)). The generated
  // grammar alone would still allow orders that name nothing ("always ->
  // jump").
  if (std::none_of(names.begin(), names.end(), [](const std::string& name) {
        return grammar::rt::IsName(name);
      })) {
    return "";
  }
  return grammar::OrdersGrammar(
      "Order",
      grammar::rt::Env{.names = {{"order_names", names}}, .counts = {}});
}

}  // namespace harness
