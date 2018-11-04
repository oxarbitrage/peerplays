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

#include "../common/database_fixture.hpp"

using namespace graphene::chain;
using namespace graphene::chain::test;

BOOST_FIXTURE_TEST_SUITE( gpos_tests, database_fixture )

   BOOST_AUTO_TEST_CASE( dividends )
   {

      //wdump((db.head_block_time()));

      ACTORS((alice)(bob));
      try
      {
         const auto& core = asset_id_type()(db);

         // all core coins are in the committee_account
         BOOST_CHECK_EQUAL(get_balance(committee_account(db), core), 1000000000000000);

         // transfer half od the total stake to alice so not all the dividends will go to the committee_account
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

         // by default core token pays dividends once per day
         const auto& dividend_data = dividend_holder_asset_object.dividend_data(db);
         BOOST_CHECK_EQUAL(*dividend_data.options.payout_interval, 2592000); //  30 days

         // update the payout interval for speed purposes of the tests
         {
            asset_update_dividend_operation op;
            op.issuer = dividend_holder_asset_object.issuer;
            op.asset_to_update = dividend_holder_asset_object.id;
            op.new_options.next_payout_time = fc::time_point::now() + fc::minutes(1);
            op.new_options.payout_interval = 60 * 60 * 24; // 1 days
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

         // advance to next maint after payout time arraives
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
            //op.vesting_seconds = 60*60*24;
            op.policy = cdd_vesting_policy_initializer{60 * 60 * 24};
            trx.operations.push_back(op);
            //op.is_gpos = true; // need to add this to the op
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
      wdump((db.head_block_time()));

      ACTORS((alice)(bob));
      try {
         auto committee = committee_account(db);
         generate_block();
         const auto& core = asset_id_type()(db);
         generate_block();

         transfer( committee_account, alice_id, core.amount( 1000 ) );
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
         generate_block();

         // add some vesting
         transaction trx;
         vesting_balance_create_operation op;
         op.fee = core.amount( 0 );
         op.creator = alice_id;
         op.owner = alice_id;
         op.amount = core.amount( 100 );
         //op.is_gpos = true; // need to add this to the op
         //op.vesting_seconds = 60*60*24;
         op.policy = cdd_vesting_policy_initializer{ 60*60*24 };

         trx.operations.push_back(op);
         set_expiration( db, trx );
         processed_transaction ptx = PUSH_TX( db,  trx, ~0  );

         // advance to HF
         while( db.head_block_time() <= HARDFORK_GPOS_TIME )
         {
            generate_block();
         }

         // no votes for witness
         auto witness = witness_id_type(1)(db);
         wdump((witness));
         // no votes for committee
         auto commi = committee_member_id_type(1)(db);
         wdump((commi));

         // advance to period start // Saturday, November 10, 2018 6:38:57 PM
         while( db.head_block_time() <= fc::time_point_sec(1541875137) )
         {
            generate_block();
         }
         generate_block();


         auto wit = witness_id_type(1);
         auto com = committee_member_id_type(1);

         // vote for something
         {
            signed_transaction trx;

            account_update_operation op;
            op.account = alice_id;
            op.new_options = alice_id(db).options;
            op.new_options->votes.insert(wit(db).vote_id);
            op.new_options->votes.insert(com(db).vote_id);
            trx.operations.push_back(op);
            trx.validate();
            set_expiration(db, trx);
            sign(trx, alice_private_key);
            PUSH_TX(db, trx);
            //trx.clear();
         }
         generate_block();
         generate_blocks(db.get_dynamic_global_properties().next_maintenance_time);
         generate_block();

         auto aw = db.get_global_properties().active_witnesses;
         wdump((aw));

         auto acm = db.get_global_properties().active_committee_members;
         wdump((acm));


         witness = witness_id_type(1)(db);
         wdump((witness));

         commi = committee_member_id_type(1)(db);
         wdump((commi));


         // advance to period start + 2 months, trying to figure out in what subperiod i am // Sunday, March 3, 2019 9:01:22 PM
         while( db.head_block_time() <= fc::time_point_sec(1551646882) )
         {
            generate_block();
         }
         generate_block();




      }
      catch (fc::exception &e) {
         edump((e.to_detail_string()));
         throw;
      }
   }
BOOST_AUTO_TEST_SUITE_END()


