/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <graphene/bookie/bookie_plugin.hpp>
#include <graphene/bookie/bookie_objects.hpp>

#include <graphene/app/impacted.hpp>

#include <graphene/chain/account_evaluator.hpp>
#include <graphene/chain/account_object.hpp>
#include <graphene/chain/config.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/evaluator.hpp>
#include <graphene/chain/operation_history_object.hpp>
#include <graphene/chain/transaction_evaluation_state.hpp>

#include <boost/algorithm/string/case_conv.hpp>

#include <fc/smart_ref_impl.hpp>
#include <fc/thread/thread.hpp>

#include <boost/polymorphic_cast.hpp>

#if 0
# ifdef DEFAULT_LOGGER
#  undef DEFAULT_LOGGER
# endif
# define DEFAULT_LOGGER "bookie_plugin"
#endif

namespace graphene { namespace bookie {

namespace detail
{
/* As a plugin, we get notified of new/changed objects at the end of every block processed.
 * For most objects, that's fine, because we expect them to always be around until the end of
 * the block.  However, with bet objects, it's possible that the user places a bet and it fills
 * and is removed during the same block, so need another strategy to detect them immediately after
 * they are created. 
 * We do this by creating a secondary index on bet_object.  We don't actually use it
 * to index any property of the bet, we just use it to register for callbacks.
 */
class persistent_bet_object_helper : public secondary_index
{
   public:
      virtual ~persistent_bet_object_helper() {}

      virtual void object_inserted(const object& obj) override;
      //virtual void object_removed( const object& obj ) override;
      //virtual void about_to_modify( const object& before ) override;
      virtual void object_modified(const object& after) override;
      void set_plugin_instance(bookie_plugin* instance) { _bookie_plugin = instance; }
   private:
      bookie_plugin* _bookie_plugin;
};

void persistent_bet_object_helper::object_inserted(const object& obj) 
{
   const bet_object& bet_obj = *boost::polymorphic_downcast<const bet_object*>(&obj);
   _bookie_plugin->database().create<persistent_bet_object>([&](persistent_bet_object& saved_bet_obj) {
      saved_bet_obj.ephemeral_bet_object = bet_obj;
   });
}
void persistent_bet_object_helper::object_modified(const object& after) 
{
   database& db = _bookie_plugin->database();
   auto& persistent_bets_by_bet_id = db.get_index_type<persistent_bet_index>().indices().get<by_bet_id>();
   const bet_object& bet_obj = *boost::polymorphic_downcast<const bet_object*>(&after);
   auto iter = persistent_bets_by_bet_id.find(bet_obj.id);
   assert (iter != persistent_bets_by_bet_id.end());
   if (iter != persistent_bets_by_bet_id.end())
      db.modify(*iter, [&](persistent_bet_object& saved_bet_obj) {
         saved_bet_obj.ephemeral_bet_object = bet_obj;
      });
}

//////////// end bet_object ///////////////////
class bookie_plugin_impl
{
   public:
      bookie_plugin_impl(bookie_plugin& _plugin)
         : _self( _plugin )
      { }
      virtual ~bookie_plugin_impl();


      /**
       *  Called After a block has been applied and committed.  The callback
       *  should not yield and should execute quickly.
       */
      void on_objects_changed(const vector<object_id_type>& changed_object_ids);

      void on_objects_new(const vector<object_id_type>& new_object_ids);
      void on_objects_removed(const vector<object_id_type>& removed_object_ids);

      /** this method is called as a callback after a block is applied
       * and will process/index all operations that were applied in the block.
       */
      void on_block_applied( const signed_block& b );

      asset get_total_matched_bet_amount_for_betting_market_group(betting_market_group_id_type group_id);

      void fill_localized_event_strings();

      std::vector<event_object> get_events_containing_sub_string(const std::string& sub_string, const std::string& language);

      graphene::chain::database& database()
      {
         return _self.database();
      }

      //                1.18.          "Washington Capitals/Chicago Blackhawks"
      typedef std::pair<event_id_type, std::string> event_string;
      struct event_string_less : public std::less<const event_string&>
      {
          bool operator()(const event_string &_left, const event_string &_right) const
          {
              return (_left.first.instance < _right.first.instance);
          }
      };

      typedef flat_set<event_string, event_string_less> event_string_set;
      //       "en"
      std::map<std::string, event_string_set> localized_event_strings;

      bookie_plugin& _self;
      flat_set<account_id_type> _tracked_accounts;
};

bookie_plugin_impl::~bookie_plugin_impl()
{
}

void bookie_plugin_impl::on_objects_new(const vector<object_id_type>& new_object_ids)
{
   //idump((new_object_ids));
   graphene::chain::database& db = database();
   auto& event_id_index = db.get_index_type<persistent_event_index>().indices().get<by_event_id>();
   auto& betting_market_group_id_index = db.get_index_type<persistent_betting_market_group_index>().indices().get<by_betting_market_group_id>();
   auto& betting_market_id_index = db.get_index_type<persistent_betting_market_index>().indices().get<by_betting_market_id>();

   for (const object_id_type& new_object_id : new_object_ids)
   {
      if (new_object_id.space() == event_id_type::space_id && 
          new_object_id.type() == event_id_type::type_id)
      {
         event_id_type new_event_id = new_object_id;
         ilog("Creating new persistent event object ${id}", ("id", new_event_id));
         db.create<persistent_event_object>([&](persistent_event_object& saved_event_obj) {
            saved_event_obj.ephemeral_event_object = new_event_id(db);
         });
      }
      else if (new_object_id.space() == betting_market_group_object::space_id && 
               new_object_id.type() == betting_market_group_object::type_id)
      {
         betting_market_group_id_type new_betting_market_group_id = new_object_id;
         ilog("Creating new persistent betting_market_group object ${id}", ("id", new_betting_market_group_id));
         db.create<persistent_betting_market_group_object>([&](persistent_betting_market_group_object& saved_betting_market_group_obj) {
            saved_betting_market_group_obj.ephemeral_betting_market_group_object = new_betting_market_group_id(db);
         });
      }
      else if (new_object_id.space() == betting_market_object::space_id && 
               new_object_id.type() == betting_market_object::type_id)
      {
         betting_market_id_type new_betting_market_id = new_object_id;
         ilog("Creating new persistent betting_market object ${id}", ("id", new_betting_market_id));
         db.create<persistent_betting_market_object>([&](persistent_betting_market_object& saved_betting_market_obj) {
            saved_betting_market_obj.ephemeral_betting_market_object = new_betting_market_id(db);
         });
      }
   }
}

void bookie_plugin_impl::on_objects_removed(const vector<object_id_type>& removed_object_ids)
{
   //idump((removed_object_ids));
}

void bookie_plugin_impl::on_objects_changed(const vector<object_id_type>& changed_object_ids)
{
   //idump((changed_object_ids));
   graphene::chain::database& db = database();
   auto& event_id_index = db.get_index_type<persistent_event_index>().indices().get<by_event_id>();
   auto& betting_market_group_id_index = db.get_index_type<persistent_betting_market_group_index>().indices().get<by_betting_market_group_id>();
   auto& betting_market_id_index = db.get_index_type<persistent_betting_market_index>().indices().get<by_betting_market_id>();

   for (const object_id_type& changed_object_id : changed_object_ids)
   {
      if (changed_object_id.space() == event_id_type::space_id && 
          changed_object_id.type() == event_id_type::type_id)
      {
         event_id_type changed_event_id = changed_object_id;
         const persistent_event_object* old_event_obj = nullptr;

         auto persistent_event_iter = event_id_index.find(changed_event_id);
         if (persistent_event_iter != event_id_index.end())
            old_event_obj = &*persistent_event_iter;

         if (old_event_obj)
         {
            ilog("Modifying persistent event object ${id}", ("id", changed_event_id));
            db.modify(*old_event_obj, [&](persistent_event_object& saved_event_obj) {
               saved_event_obj.ephemeral_event_object = changed_event_id(db);
            });
         }
         else
            elog("Received change notification on event ${event_id} that we didn't know about", ("event_id", changed_event_id));
      }
      else if (changed_object_id.space() == betting_market_group_object::space_id && 
               changed_object_id.type() == betting_market_group_object::type_id)
      {
         betting_market_group_id_type changed_betting_market_group_id = changed_object_id;
         const persistent_betting_market_group_object* old_betting_market_group_obj = nullptr;

         auto persistent_betting_market_group_iter = betting_market_group_id_index.find(changed_betting_market_group_id);
         if (persistent_betting_market_group_iter != betting_market_group_id_index.end())
            old_betting_market_group_obj = &*persistent_betting_market_group_iter;
         
         if (old_betting_market_group_obj)
         {
            ilog("Modifying persistent betting_market_group object ${id}", ("id", changed_betting_market_group_id));
            db.modify(*old_betting_market_group_obj, [&](persistent_betting_market_group_object& saved_betting_market_group_obj) {
               saved_betting_market_group_obj.ephemeral_betting_market_group_object = changed_betting_market_group_id(db);
            });
         }
         else
            elog("Received change notification on betting market group ${betting_market_group_id} that we didn't know about", 
                 ("betting_market_group_id", changed_betting_market_group_id));
      }
      else if (changed_object_id.space() == betting_market_object::space_id && 
               changed_object_id.type() == betting_market_object::type_id)
      {
         betting_market_id_type changed_betting_market_id = changed_object_id;
         const persistent_betting_market_object* old_betting_market_obj = nullptr;

         auto persistent_betting_market_iter = betting_market_id_index.find(changed_betting_market_id);
         if (persistent_betting_market_iter != betting_market_id_index.end())
            old_betting_market_obj = &*persistent_betting_market_iter;
         
         if (old_betting_market_obj)
         {
            ilog("Modifying persistent betting_market object ${id}", ("id", changed_betting_market_id));
            db.modify(*old_betting_market_obj, [&](persistent_betting_market_object& saved_betting_market_obj) {
               saved_betting_market_obj.ephemeral_betting_market_object = changed_betting_market_id(db);
            });
         }
         else
            elog("Received change notification on betting market ${betting_market_id} that we didn't know about", 
                 ("betting_market_id", changed_betting_market_id));
      }
   }
}

void bookie_plugin_impl::on_block_applied( const signed_block& )
{
   graphene::chain::database& db = database();
   const vector<optional<operation_history_object> >& hist = db.get_applied_operations();
   for( const optional<operation_history_object>& o_op : hist )
   {
      if( !o_op.valid() )
         continue;

      const operation_history_object& op = *o_op;
      if( op.op.which() == operation::tag<bet_matched_operation>::value )
      {
         const bet_matched_operation& bet_matched_op = op.op.get<bet_matched_operation>();
         //idump((bet_matched_op));
         const asset& amount_bet = bet_matched_op.amount_bet;
         // object may no longer exist
         //const bet_object& bet = bet_matched_op.bet_id(db);
         auto& persistent_bets_by_bet_id = db.get_index_type<persistent_bet_index>().indices().get<by_bet_id>();
         auto bet_iter = persistent_bets_by_bet_id.find(bet_matched_op.bet_id);
         assert(bet_iter != persistent_bets_by_bet_id.end());
         if (bet_iter != persistent_bets_by_bet_id.end())
         {
            db.modify(*bet_iter, [&]( persistent_bet_object& obj ) {
               obj.amount_matched += amount_bet.amount;
            });
            const bet_object& bet_obj = bet_iter->ephemeral_bet_object;

            const betting_market_object& betting_market = bet_obj.betting_market_id(db); // TODO: this needs to look at the persistent version
            const betting_market_group_object& betting_market_group = betting_market.group_id(db); // TODO: as does this
            db.modify( betting_market_group, [&]( betting_market_group_object& obj ){
               obj.total_matched_bets_amount += amount_bet.amount;
            });
         }
      }
      else if( op.op.which() == operation::tag<event_create_operation>::value )
      {
         FC_ASSERT(op.result.which() == operation_result::tag<object_id_type>::value);
         //object_id_type object_id = op.result.get<object_id_type>();
         event_id_type object_id = op.result.get<object_id_type>();
         FC_ASSERT( db.find_object(object_id), "invalid event specified" );
         const event_create_operation& event_create_op = op.op.get<event_create_operation>();
         for(const std::pair<std::string, std::string>& pair : event_create_op.name)
            localized_event_strings[pair.first].insert(event_string(object_id, pair.second));
      }
      else if( op.op.which() == operation::tag<event_update_operation>::value )
      {
         const event_update_operation& event_create_op = op.op.get<event_update_operation>();
         if (!event_create_op.new_name.valid())
            continue;
         event_id_type event_id = event_create_op.event_id;
         for(const std::pair<std::string, std::string>& pair : *event_create_op.new_name)
         {
            // try insert
            std::pair<event_string_set::iterator, bool> result =
               localized_event_strings[pair.first].insert(event_string(event_id, pair.second));
            if (!result.second)
               //  update string only
               result.first->second = pair.second;
         }
      }
   }
}

void bookie_plugin_impl::fill_localized_event_strings()
{
       graphene::chain::database& db = database();
       const auto& event_index = db.get_index_type<event_object_index>().indices().get<by_id>();
       auto event_itr = event_index.cbegin();
       while (event_itr != event_index.cend())
       {
           const event_object& event_obj = *event_itr;
           ++event_itr;
           for(const std::pair<std::string, std::string>& pair : event_obj.name)
           {
                localized_event_strings[pair.first].insert(event_string(event_obj.id, pair.second));
           }
       }
}

std::vector<event_object> bookie_plugin_impl::get_events_containing_sub_string(const std::string& sub_string, const std::string& language)
{
   graphene::chain::database& db = database();
   std::vector<event_object> events;
   if (localized_event_strings.find(language) != localized_event_strings.end())
   {
      std::string lower_case_sub_string = boost::algorithm::to_lower_copy(sub_string);
      const event_string_set& language_set = localized_event_strings[language];
      for (const event_string& pair : language_set)
      {
         std::string lower_case_string = boost::algorithm::to_lower_copy(pair.second);
         if (lower_case_string.find(lower_case_sub_string) != std::string::npos)
            events.push_back(pair.first(db));
      }
   }
   return events;
}

asset bookie_plugin_impl::get_total_matched_bet_amount_for_betting_market_group(betting_market_group_id_type group_id)
{
   graphene::chain::database& db = database();
   FC_ASSERT( db.find_object(group_id), "Invalid betting market group specified" );
   const betting_market_group_object& betting_market_group =  group_id(db);
   return asset(betting_market_group.total_matched_bets_amount, betting_market_group.asset_id);
}
} // end namespace detail

bookie_plugin::bookie_plugin() :
   my( new detail::bookie_plugin_impl(*this) )
{
}

bookie_plugin::~bookie_plugin()
{
}

std::string bookie_plugin::plugin_name()const
{
   return "bookie";
}

void bookie_plugin::plugin_set_program_options(boost::program_options::options_description& cli, 
                                               boost::program_options::options_description& cfg)
{
   //cli.add_options()
   //      ("track-account", boost::program_options::value<std::vector<std::string>>()->composing()->multitoken(), "Account ID to track history for (may specify multiple times)")
   //      ;
   //cfg.add(cli);
}

void bookie_plugin::plugin_initialize(const boost::program_options::variables_map& options)
{
    ilog("bookie plugin: plugin_startup() begin");
    database().applied_block.connect( [&]( const signed_block& b){ my->on_block_applied(b); } );
    database().changed_objects.connect([&](const vector<object_id_type>& changed_object_ids, const fc::flat_set<graphene::chain::account_id_type>& impacted_accounts){ my->on_objects_changed(changed_object_ids); });
    database().new_objects.connect([this](const vector<object_id_type>& ids, const flat_set<account_id_type>& impacted_accounts) { my->on_objects_new(ids); });
    database().removed_objects.connect([this](const vector<object_id_type>& ids, const vector<const object*>& objs, const flat_set<account_id_type>& impacted_accounts) { my->on_objects_removed(ids); });


    //auto event_index =
    database().add_index<primary_index<detail::persistent_event_index> >();
    database().add_index<primary_index<detail::persistent_betting_market_group_index> >();
    database().add_index<primary_index<detail::persistent_betting_market_index> >();
    database().add_index<primary_index<detail::persistent_bet_index> >();
    const primary_index<bet_object_index>& bet_object_idx = database().get_index_type<primary_index<bet_object_index> >();
    primary_index<bet_object_index>& nonconst_bet_object_idx = const_cast<primary_index<bet_object_index>&>(bet_object_idx);
    detail::persistent_bet_object_helper* persistent_bet_object_helper_index = nonconst_bet_object_idx.add_secondary_index<detail::persistent_bet_object_helper>();
    persistent_bet_object_helper_index->set_plugin_instance(this);

    ilog("bookie plugin: plugin_startup() end");
 }

void bookie_plugin::plugin_startup()
{
   ilog("bookie plugin: plugin_startup()");
    my->fill_localized_event_strings();
}

flat_set<account_id_type> bookie_plugin::tracked_accounts() const
{
   return my->_tracked_accounts;
}

asset bookie_plugin::get_total_matched_bet_amount_for_betting_market_group(betting_market_group_id_type group_id)
{
     ilog("bookie plugin: get_total_matched_bet_amount_for_betting_market_group($group_id)", ("group_d", group_id));
     return my->get_total_matched_bet_amount_for_betting_market_group(group_id);
}
std::vector<event_object> bookie_plugin::get_events_containing_sub_string(const std::string& sub_string, const std::string& language)
{
    ilog("bookie plugin: get_events_containing_sub_string(${sub_string}, ${language})", (sub_string)(language));
    return my->get_events_containing_sub_string(sub_string, language);
}

} }

