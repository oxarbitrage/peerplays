/*
 * Copyright (c) 2018 oxarbitrage and contributors.
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
#include <boost/test/unit_test.hpp>
#include <graphene/chain/hardfork.hpp>
#include <graphene/chain/database.hpp>

#include <graphene/chain/balance_object.hpp>
#include <graphene/chain/witness_object.hpp>
#include <graphene/chain/committee_member_object.hpp>
#include <graphene/chain/vesting_balance_object.hpp>
#include <graphene/chain/worker_object.hpp>

#include "../common/database_fixture.hpp"

using namespace graphene::chain;
using namespace graphene::chain::test;

BOOST_FIXTURE_TEST_SUITE( gpos_tests, database_fixture )

BOOST_AUTO_TEST_CASE( dividends )
{
   ACTORS((alice)(bob));
   try
   {
      const auto& core = asset_id_type()(db);

      // all core coins are in the committee_account
      BOOST_CHECK_EQUAL(get_balance(committee_account(db), core), 1000000000000000);

      // transfer half of the total stake to alice so not all the dividends will go to the committee_account
      transfer( committee_account, alice_id, core.amount( 500000000000000 ) );
      generate_block();

      // send some to bob
      transfer( committee_account, bob_id, core.amount( 1000 ) );
      generate_block();

      // committee balance
      BOOST_CHECK_EQUAL(get_balance(committee_account(db), core), 499999999999000);

      // alice balance
      BOOST_CHECK_EQUAL(get_balance(alice_id(db), core), 500000000000000);

      // bob balance
      BOOST_CHECK_EQUAL(get_balance(bob_id(db), core), 1000);

      // get core asset object
      const auto& dividend_holder_asset_object = get_asset("PPY");

      // by default core token pays dividends once per month
      const auto& dividend_data = dividend_holder_asset_object.dividend_data(db);
      BOOST_CHECK_EQUAL(*dividend_data.options.payout_interval, 2592000); //  30 days

      // update the payout interval for speed purposes of the test
      {
         asset_update_dividend_operation op;
         op.issuer = dividend_holder_asset_object.issuer;
         op.asset_to_update = dividend_holder_asset_object.id;
         op.new_options.next_payout_time = fc::time_point::now() + fc::minutes(1);
         op.new_options.payout_interval = 60 * 60 * 24; // 1 day
         trx.operations.push_back(op);
         set_expiration(db, trx);
         PUSH_TX(db, trx, ~0);
         trx.operations.clear();
      }
      generate_block();

      BOOST_CHECK_EQUAL(*dividend_data.options.payout_interval, 86400); // 1 day now

      // get the dividend distribution account
      const account_object& dividend_distribution_account = dividend_data.dividend_distribution_account(db);

      // transfering some coins to distribution account.
      // simulating the blockchain haves some dividends to pay.
      transfer( committee_account, dividend_distribution_account.id, core.amount( 100 ) );
      generate_block();

      // committee balance
      BOOST_CHECK_EQUAL(get_balance(committee_account(db), core), 499999999998900 );

      // distribution account balance
      BOOST_CHECK_EQUAL(get_balance(dividend_distribution_account, core), 100);

      // get when is the next payout time as we need to advance there
      auto next_payout_time = dividend_data.options.next_payout_time;

      // advance to next payout
      if(next_payout_time.valid()) {
         while (db.head_block_time() <= *next_payout_time) {
            generate_block();
         }
      }

      // advance to next maint after payout time arrives
      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      // check balances now, dividends are paid "normally"
      BOOST_CHECK_EQUAL(get_balance(committee_account(db), core), 499999999998949 );
      BOOST_CHECK_EQUAL(get_balance(alice_id(db), core), 500000000000050 );
      BOOST_CHECK_EQUAL(get_balance(bob_id(db), core), 1000 );
      BOOST_CHECK_EQUAL(get_balance(dividend_distribution_account, core), 1);

      // advance to hardfork
      generate_blocks( HARDFORK_GPOS_TIME );

      // advance to next maint
      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      // send 99 to the distribution account so it will have 100 PPY again on it
      transfer( committee_account, dividend_distribution_account.id, core.amount( 99 ) );
      generate_block();

      // get when is the next payout time as we need to advance there
      next_payout_time = dividend_data.options.next_payout_time;

      // advance to next payout
      if(next_payout_time.valid()) {
         while (db.head_block_time() <= *next_payout_time) {
            generate_block();
         }
      }

      // make sure not dividends were paid
      BOOST_CHECK_EQUAL(get_balance(committee_account(db), core), 499999999998850 );
      BOOST_CHECK_EQUAL(get_balance(alice_id(db), core), 500000000000050 );
      BOOST_CHECK_EQUAL(get_balance(bob_id(db), core), 1000 );
      BOOST_CHECK_EQUAL(get_balance(dividend_distribution_account, core), 100);

      // lets create a vesting and see what happens
      {
         transaction trx;
         vesting_balance_create_operation op;
         op.fee = core.amount(0);
         op.creator = bob_id;
         op.owner = bob_id;
         op.amount = core.amount(100);
         op.balance_type = vesting_balance_type::gpos;
         //op.vesting_seconds = 60*60*24;
         op.policy = cdd_vesting_policy_initializer{60 * 60 * 24};
         trx.operations.push_back(op);
         set_expiration(db, trx);
         processed_transaction ptx = PUSH_TX(db, trx, ~0);
      }
      generate_block();

      // make sure the vesting balance is there
      vector<vesting_balance_object> bob_vesting_balances;
      auto vesting_range = db.get_index_type<vesting_balance_index>().indices().get<by_account>().equal_range(bob_id);
      std::for_each(vesting_range.first, vesting_range.second,
                    [&bob_vesting_balances](const vesting_balance_object& balance) {
                       bob_vesting_balances.emplace_back(balance);
                    });
      BOOST_CHECK_EQUAL(bob_vesting_balances[0].balance.amount.value, 100 );

      // check balances
      BOOST_CHECK_EQUAL(get_balance(bob_id(db), core), 900 );
      BOOST_CHECK_EQUAL(get_balance(dividend_distribution_account, core), 100);

      // advance to next payout
      next_payout_time = dividend_data.options.next_payout_time;
      if(next_payout_time.valid()) {
         while (db.head_block_time() <= *next_payout_time) {
            generate_block();
         }
      }
      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

      // check balances, payout paid to bob
      BOOST_CHECK_EQUAL(get_balance(bob_id(db), core), 1000 );
      BOOST_CHECK_EQUAL(get_balance(dividend_distribution_account, core), 0);
   }
   catch (fc::exception& e)
   {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( voting )
{
   ACTORS((alice)(bob));
   try {
      const auto& core = asset_id_type()(db);

      // send some asset to alice
      transfer( committee_account, alice_id, core.amount( 1000 ) );
      generate_block();

      // default maintenance_interval is 1 day
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.maintenance_interval, 86400);

      // add some vesting to alice
      {
         transaction trx;
         vesting_balance_create_operation op;
         op.fee = core.amount(0);
         op.creator = alice_id;
         op.owner = alice_id;
         op.amount = core.amount(100);
         op.balance_type = vesting_balance_type::gpos;
         //op.vesting_seconds = 60*60*24;
         op.policy = cdd_vesting_policy_initializer{60 * 60 * 24};
         trx.operations.push_back(op);
         set_expiration(db, trx);
         processed_transaction ptx = PUSH_TX(db, trx, ~0);
      }

      // advance to HF
      while( db.head_block_time() <= HARDFORK_GPOS_TIME )
      {
         generate_block();
      }

      // update default gpos global parameters to make this thing faster
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_period, 15552000);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_subperiod, 2592000);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, HARDFORK_GPOS_TIME.sec_since_epoch());

      auto now = db.head_block_time().sec_since_epoch();
      db.modify(db.get_global_properties(), [now](global_property_object& p) {
         p.parameters.vesting_period = 518400; // 60x60x24x6 = 6 days
         p.parameters.vesting_subperiod = 86400; // 60x60x24 = 1 day
         p.parameters.period_start =  now; // now
      });

      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_period, 518400);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_subperiod, 86400);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);
      // end global changes

      generate_block();

      // no votes for witness 1
      auto witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 0);

      // no votes for witness 2
      auto witness2 = witness_id_type(2)(db);
      BOOST_CHECK_EQUAL(witness2.total_votes, 0);

      /* commitee haves some votes by default so lets work with witnesses, probably need test for workers as well */

      // vote for witness1
      {
         signed_transaction trx;
         account_update_operation op;
         op.account = alice_id;
         op.new_options = alice_id(db).options;
         op.new_options->votes.insert(witness1.vote_id);
         //op.new_options->votes.insert(committee_member1.vote_id);
         trx.operations.push_back(op);
         trx.validate();
         set_expiration(db, trx);
         sign(trx, alice_private_key);
         PUSH_TX(db, trx);
         //trx.clear();
      }
      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

      auto alice_last_time_voted = alice_id(db).statistics(db).last_vote_time;

      // vote decay as time pass
      witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 100);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 83);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 66);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 50);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 33);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 16);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 0);

      // as more time pass vote from alice worth 0
      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      witness1 = witness_id_type(1)(db);
      BOOST_CHECK_EQUAL(witness1.total_votes, 0);
   }
   catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( rolling_period_start )
{
   // period start need to roll automatically after HF
   try {

      // advance to HF
      while( db.head_block_time() <= HARDFORK_GPOS_TIME )
      {
         generate_block();
      }

      // update default gpos global parameters to make this thing faster
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_period, 15552000);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_subperiod, 2592000);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, HARDFORK_GPOS_TIME.sec_since_epoch());

      auto now = db.head_block_time().sec_since_epoch();
      db.modify(db.get_global_properties(), [now](global_property_object& p) {
         p.parameters.vesting_period = 518400; // 60x60x24x6 = 6 days
         p.parameters.vesting_subperiod = 86400; // 60x60x24 = 1 day
         p.parameters.period_start =  now; // now
      });

      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_period, 518400);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_subperiod, 86400);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);
      // end global changes

      // moving outside period:
      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      // rolling is here so getting the new now
      now = db.head_block_time().sec_since_epoch();
      generate_block();

      // period start rolled
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);
   }
   catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}
BOOST_AUTO_TEST_CASE( worker_dividends_voting )
{
   try {

      // advance to HF
      while( db.head_block_time() <= HARDFORK_GPOS_TIME )
      {
         generate_block();
      }

      // update default gpos global parameters to 4 days
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_period, 15552000);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_subperiod, 2592000);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, HARDFORK_GPOS_TIME.sec_since_epoch());

      auto now = db.head_block_time().sec_since_epoch();
      db.modify(db.get_global_properties(), [now](global_property_object& p) {
         p.parameters.vesting_period = 345600; // 60x60x24x4 = 4 days
         p.parameters.vesting_subperiod = 86400; // 60x60x24 = 1 day
         p.parameters.period_start =  now; // now
      });

      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_period, 345600);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.vesting_subperiod, 86400);
      BOOST_CHECK_EQUAL(db.get_global_properties().parameters.period_start, now);
      // end global changes

      generate_block();
      set_expiration(db, trx);
      const auto& core = asset_id_type()(db);

      // get core asset object
      const auto& dividend_holder_asset_object = get_asset("PPY");

      // by default core token pays dividends once per month
      const auto& dividend_data = dividend_holder_asset_object.dividend_data(db);
      BOOST_CHECK_EQUAL(*dividend_data.options.payout_interval, 2592000); //  30 days

      // update the payout interval to 1 day for speed purposes of the test
      {
         asset_update_dividend_operation op;
         op.issuer = dividend_holder_asset_object.issuer;
         op.asset_to_update = dividend_holder_asset_object.id;
         op.new_options.next_payout_time = fc::time_point::now() + fc::minutes(1);
         op.new_options.payout_interval = 60 * 60 * 24; // 1 day
         trx.operations.push_back(op);
         set_expiration(db, trx);
         PUSH_TX(db, trx, ~0);
         trx.operations.clear();
      }
      generate_block();

      // get the dividend distribution account
      const account_object& dividend_distribution_account = dividend_data.dividend_distribution_account(db);

      // transfering some coins to distribution account.
      // simulating the blockchain haves some dividends to pay.
      transfer( committee_account, dividend_distribution_account.id, core.amount( 100 ) );
      generate_block();

      ACTORS((nathan)(voter1)(voter2)(voter3));

      transfer( committee_account, nathan_id, core.amount( 1000 ) );
      transfer( committee_account, voter1_id, core.amount( 1000 ) );
      transfer( committee_account, voter2_id, core.amount( 1000 ) );

      generate_block();

      upgrade_to_lifetime_member(nathan_id);

      // create a worker
      {
         worker_create_operation op;
         op.owner = nathan_id;
         op.daily_pay = 10;
         op.initializer = vesting_balance_worker_initializer(1);
         op.work_begin_date = db.head_block_time() + fc::days(1);
         op.work_end_date = op.work_begin_date + fc::days(6);
         trx.clear();
         trx.operations.push_back(op);
         sign( trx, nathan_private_key );
         PUSH_TX( db, trx );
      }


      auto worker = worker_id_type()(db);
      /* worker checks ?
      BOOST_CHECK(worker.worker_account == nathan_id);
      BOOST_CHECK(worker.daily_pay == 10);
      BOOST_CHECK(worker.work_begin_date == db.head_block_time() + fc::days(1));
      BOOST_CHECK(worker.work_end_date == db.head_block_time() + fc::days(7));
      BOOST_CHECK(worker.vote_for.type() == vote_id_type::worker);
      BOOST_CHECK(worker.vote_against.type() == vote_id_type::worker);

      const vesting_balance_object& balance = worker.worker.get<vesting_balance_worker_type>().balance(db);
      BOOST_CHECK(balance.owner == nathan_id);
      BOOST_CHECK(balance.balance == asset(0));
      BOOST_CHECK(balance.policy.get<cdd_vesting_policy>().vesting_seconds == fc::days(1).to_seconds());
      */

      // add some vesting to voter1
      {
         transaction trx;
         vesting_balance_create_operation op;
         op.fee = core.amount(0);
         op.creator = voter1_id;
         op.owner = voter1_id;
         op.amount = core.amount(100);
         op.balance_type = vesting_balance_type::gpos;
         //op.vesting_seconds = 60*60*24;
         op.policy = cdd_vesting_policy_initializer{60 * 60 * 24};

         trx.operations.push_back(op);
         set_expiration(db, trx);
         processed_transaction ptx = PUSH_TX(db, trx, ~0);
      }
      // check vesting

      // add some vesting to voter2
      {
         transaction trx;
         vesting_balance_create_operation op;
         op.fee = core.amount(0);
         op.creator = voter2_id;
         op.owner = voter2_id;
         op.amount = core.amount(100);
         op.balance_type = vesting_balance_type::gpos;
         op.policy = cdd_vesting_policy_initializer{60 * 60 * 24};
         trx.operations.push_back(op);
         set_expiration(db, trx);
         processed_transaction ptx = PUSH_TX(db, trx, ~0);
      }

      /* vesting chexks, needed?
      // check vesting at voter2
      // make sure the vesting balance is there
      vector<vesting_balance_object> voter2_vesting_balances;
      auto vesting_range = db.get_index_type<vesting_balance_index>().indices().get<by_account>().equal_range(voter2_id);
      std::for_each(vesting_range.first, vesting_range.second,
                    [&voter2_vesting_balances](const vesting_balance_object& balance) {
                       voter2_vesting_balances.emplace_back(balance);
                    });
      BOOST_CHECK_EQUAL(voter2_vesting_balances[0].balance.amount.value, 100 );
      */
      generate_block();

      // vote against is not possible after HARDFORK_607_TIME
      // samp[les in participation rewards are not reproducible

      // vote for worker
      {
         signed_transaction trx;
         account_update_operation op;
         op.account = voter1_id;
         op.new_options = voter1_id(db).options;
         op.new_options->votes.insert(worker.vote_for);
         trx.operations.push_back(op);
         trx.validate();
         set_expiration(db, trx);
         sign(trx, voter1_private_key);
         PUSH_TX(db, trx);
         //trx.clear();
      }

      // first maint pass, coefficient will be 1
      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      generate_block();

      // vote decay as time pass
      //auto witness1 = witness_id_type(1)(db);
      worker = worker_id_type()(db);
      BOOST_CHECK_EQUAL(worker.total_votes_for, 100);


      // here dividends are paid to voter1 and voter2
      // voter1 get paid dividends full dividend share as coefficent is at 1 here
      BOOST_CHECK_EQUAL(get_balance(voter1_id(db), core), 950);

      // voter2 get  paid dividends
      BOOST_CHECK_EQUAL(get_balance(voter2_id(db), core), 950);

      //BOOST_CHECK_EQUAL(worker.total_votes_against, 0);

      // send some asset to the reserve pool for the worker
      {
         asset_reserve_operation op;
         op.payer = account_id_type();
         op.amount_to_reserve = asset(GRAPHENE_MAX_SHARE_SUPPLY/2);
         trx.operations.push_back(op);
         trx.validate();
         set_expiration(db, trx);
         PUSH_TX( db, trx, ~0 );
         trx.clear();
      }

      BOOST_CHECK_EQUAL(worker_id_type()(db).worker.get<vesting_balance_worker_type>().balance(db).balance.amount.value, 0);

      // worker is getting paid
      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
      BOOST_CHECK_EQUAL(worker_id_type()(db).worker.get<vesting_balance_worker_type>().balance(db).balance.amount.value, 10);
      generate_block();

      // second maint pass, coefficient will be 0.75
      worker = worker_id_type()(db);
      BOOST_CHECK_EQUAL(worker.total_votes_for, 75);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

      worker = worker_id_type()(db);
      BOOST_CHECK_EQUAL(worker.total_votes_for, 50);

      //is the worker still active?
      // what is the worker thereshold?
      //wdump((worker));

      transfer( committee_account, dividend_distribution_account.id, core.amount( 100 ) );
      generate_block();

      BOOST_CHECK_EQUAL(get_balance(committee_account(db), core), 499999999996800);

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

      worker = worker_id_type()(db);
      BOOST_CHECK_EQUAL(worker.total_votes_for, 25);

      BOOST_CHECK_EQUAL(get_balance(committee_account(db), core), 499999999996876);

      // here voter1 and voter2 get paid again but less money for vesting coefficient
      BOOST_CHECK_EQUAL(get_balance(voter1_id(db), core), 962);
      BOOST_CHECK_EQUAL(get_balance(voter2_id(db), core), 962);



   }
   catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_CASE( account_multiple_vesting )
{
   try {

      ACTORS((sam)(patty));

      const auto& core = asset_id_type()(db);

      transfer( committee_account, sam_id, core.amount( 300 ) );
      transfer( committee_account, patty_id, core.amount( 100 ) );

      // advance to HF
      while( db.head_block_time() <= HARDFORK_GPOS_TIME )
      {
         generate_block();
      }
      // add some vesting to voter2
      {
         transaction trx;
         vesting_balance_create_operation op;
         op.fee = core.amount(0);
         op.creator = sam_id;
         op.owner = sam_id;
         op.amount = core.amount(100);
         op.balance_type = vesting_balance_type::gpos;
         op.policy = cdd_vesting_policy_initializer{60 * 60 * 24};
         trx.operations.push_back(op);
         set_expiration(db, trx);
         processed_transaction ptx = PUSH_TX(db, trx, ~0);
      }
      // check vesting at voter2

      // have another balance with 200 more
      {
         transaction trx;
         vesting_balance_create_operation op;
         op.fee = core.amount(0);
         op.creator = sam_id;
         op.owner = sam_id;
         op.amount = core.amount(200);
         op.balance_type = vesting_balance_type::gpos;
         op.policy = cdd_vesting_policy_initializer{60 * 60 * 24};
         trx.operations.push_back(op);
         set_expiration(db, trx);
         processed_transaction ptx = PUSH_TX(db, trx, ~0);
      }


      // patty also have vesting balance
      {
         transaction trx;
         vesting_balance_create_operation op;
         op.fee = core.amount(0);
         op.creator = patty_id;
         op.owner = patty_id;
         op.amount = core.amount(100);
         op.balance_type = vesting_balance_type::gpos;
         op.policy = cdd_vesting_policy_initializer{60 * 60 * 24};
         trx.operations.push_back(op);
         set_expiration(db, trx);
         processed_transaction ptx = PUSH_TX(db, trx, ~0);
      }
      // check vesting at patty

      // get core asset object
      const auto& dividend_holder_asset_object = get_asset("PPY");

      // by default core token pays dividends once per month
      const auto& dividend_data = dividend_holder_asset_object.dividend_data(db);
      BOOST_CHECK_EQUAL(*dividend_data.options.payout_interval, 2592000); //  30 days

      // update the payout interval for speed purposes of the test
      {
         asset_update_dividend_operation op;
         op.issuer = dividend_holder_asset_object.issuer;
         op.asset_to_update = dividend_holder_asset_object.id;
         op.new_options.next_payout_time = fc::time_point::now() + fc::minutes(1);
         op.new_options.payout_interval = 60 * 60 * 24; // 1 day
         trx.operations.push_back(op);
         set_expiration(db, trx);
         PUSH_TX(db, trx, ~0);
         trx.operations.clear();
      }
      generate_block();

      // get the dividend distribution account
      const account_object& dividend_distribution_account = dividend_data.dividend_distribution_account(db);

      // transfering some coins to distribution account.
      // simulating the blockchain haves some dividends to pay.
      transfer( committee_account, dividend_distribution_account.id, core.amount( 100 ) );
      generate_block();

      generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);

      // sam get paid dividends
      BOOST_CHECK_EQUAL(get_balance(sam_id(db), core), 75);

      // patty also
      BOOST_CHECK_EQUAL(get_balance(patty_id(db), core), 25);

   }
   catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}
BOOST_AUTO_TEST_CASE( competing_proposals )
{
   try {


   }
   catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}
BOOST_AUTO_TEST_CASE( proxy_voting )
{
   try {

   }
   catch (fc::exception &e) {
      edump((e.to_detail_string()));
      throw;
   }
}

BOOST_AUTO_TEST_SUITE_END()
