#pragma once

#include <memory>
#include <string>

#include <fc/api.hpp>
#include <fc/variant_object.hpp>

#include <graphene/chain/protocol/types.hpp>
#include <graphene/chain/protocol/asset.hpp>
#include <graphene/chain/event_object.hpp>

using namespace graphene::chain;

namespace graphene { namespace app {
   class application;
} }

namespace graphene { namespace bookie {

namespace detail {
   class bookie_api_impl;
}

struct order_bin {
   graphene::chain::share_type amount_to_bet;
   graphene::chain::bet_multiplier_type backer_multiplier;
};

struct binned_order_book {
   std::vector<order_bin> aggregated_back_bets;
   std::vector<order_bin> aggregated_lay_bets;
};

class bookie_api
{
   public:
      bookie_api(graphene::app::application& app);

      /**
       * Returns the current order book, binned according to the given precision.
       * precision = 1 means bin using one decimal place.  for backs, (1 - 1.1], (1.1 - 1.2], etc.
       * precision = 2 would bin on (1 - 1.01], (1.01 - 1.02]
       */
      binned_order_book get_binned_order_book(graphene::chain::betting_market_id_type betting_market_id, int32_t precision);
      asset get_total_matched_bet_amount_for_betting_market_group(betting_market_group_id_type group_id);
      std::vector<event_object> get_events_containing_sub_string(const std::string& sub_string, const std::string& language);
      fc::variants get_objects(const vector<object_id_type>& ids)const;

      std::shared_ptr<detail::bookie_api_impl> my;
};

} }

FC_REFLECT(graphene::bookie::order_bin, (amount_to_bet)(backer_multiplier))
FC_REFLECT(graphene::bookie::binned_order_book, (aggregated_back_bets)(aggregated_lay_bets))

FC_API(graphene::bookie::bookie_api,
       (get_binned_order_book)
       (get_total_matched_bet_amount_for_betting_market_group)
       (get_events_containing_sub_string)
       (get_objects)
     )

