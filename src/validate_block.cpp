/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/validate_block.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>
#include <boost/date_time.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/block.hpp>
#include <bitcoin/blockchain/checkpoint.hpp>
#include <bitcoin/blockchain/validate_transaction.hpp>

namespace libbitcoin {
namespace blockchain {

// To improve readability.
#define RETURN_IF_STOPPED() \
if (stopped()) \
    return error::service_stopped

using boost::posix_time::ptime;
using boost::posix_time::from_time_t;
using boost::posix_time::second_clock;
using boost::posix_time::hours;

// Max block size is 1,000,000.
static constexpr uint32_t max_block_size = 1000000;

// Maximum signature operations per block is 20,000.
static constexpr uint32_t max_block_script_sig_operations = 
    max_block_size / 50;

// BIP30 exception blocks.
// see: github.com/bitcoin/bips/blob/master/bip-0030.mediawiki#specification
static const config::checkpoint mainnet_block_91842 =
{ "00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec", 91842 };
static const config::checkpoint mainnet_block_91880 =
{ "00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721", 91880 };

// The maximum height of version 1 blocks.
// Is this correct, looks like it should be 227835?
// see: github.com/bitcoin/bips/blob/master/bip-0034.mediawiki#result
static constexpr uint64_t max_version1_height = 237370;

// Target readjustment every 2 weeks (in seconds).
static constexpr uint64_t target_timespan = 2 * 7 * 24 * 60 * 60;

// Aim for blocks every 10 mins (in seconds).
static constexpr uint64_t target_spacing = 10 * 60;

// Value used to define retargeting range constraint.
static constexpr uint64_t retargeting_factor = 4;

// Two weeks worth of blocks (count of blocks).
static constexpr uint64_t retargeting_interval = 
    target_timespan / target_spacing;

// BIP30 exception blocks (with duplicate transactions).
// see: github.com/bitcoin/bips/blob/master/bip-0030.mediawiki#specification
bool validate_block::is_special_duplicate(const chain::block& block,
    size_t height)
{
    if (height != mainnet_block_91842.height() &&
        height != mainnet_block_91880.height())
        return false;

    const auto hash = block.header.hash();
    return 
        hash == mainnet_block_91842.hash() || 
        hash == mainnet_block_91880.hash();
}

// The nullptr option is for backward compatibility only.
validate_block::validate_block(size_t height, const chain::block& block,
    bool testnet, const config::checkpoint::list& checks,
    stopped_callback callback)
  : testnet_(testnet),
    height_(height),
    current_block_(block),
    checkpoints_(checks),
    stop_callback_(callback == nullptr ? [](){ return false; } : callback)
{
}

bool validate_block::stopped() const
{
    return stop_callback_();
}

code validate_block::check_block() const
{
    // These are checks that are independent of the blockchain
    // that can be validated before saving an orphan block.

    const auto& transactions = current_block_.transactions;

    if (transactions.empty() || transactions.size() > max_block_size ||
        current_block_.serialized_size() > max_block_size)
    {
        return error::size_limits;
    }

    const auto& header = current_block_.header;
    const auto hash = header.hash();

    if (!is_valid_proof_of_work(hash, header.bits))
        return error::proof_of_work;

    RETURN_IF_STOPPED();

    if (!is_valid_time_stamp(header.timestamp))
        return error::futuristic_timestamp;

    RETURN_IF_STOPPED();

    if (!transactions.front().is_coinbase())
        return error::first_not_coinbase;

    for (auto it = ++transactions.begin(); it != transactions.end(); ++it)
    {
        RETURN_IF_STOPPED();

        if ((*it).is_coinbase())
            return error::extra_coinbases;
    }

    for (const auto& tx: transactions)
    {
        RETURN_IF_STOPPED();

        const auto ec = validate_transaction::check_transaction(tx);
        if (ec)
            return ec;
    }

    RETURN_IF_STOPPED();

    if (!is_distinct_tx_set(transactions))
        return error::duplicate;

    RETURN_IF_STOPPED();

    const auto sigops = legacy_sigops_count(transactions);
    if (sigops > max_block_script_sig_operations)
        return error::too_many_sigs;

    RETURN_IF_STOPPED();

    if (header.merkle != chain::block::generate_merkle_root(transactions))
        return error::merkle_mismatch;

    return error::success;
}

bool validate_block::is_distinct_tx_set(const chain::transaction::list& txs)
{
    // We define distinctness by transaction hash.
    const auto hasher = [](const chain::transaction& transaction)
    {
        return transaction.hash();
    };

    std::vector<hash_digest> hashes(txs.size());
    std::transform(txs.begin(), txs.end(), hashes.begin(), hasher);
    auto distinct_end = std::unique(hashes.begin(), hashes.end());
    return distinct_end == hashes.end();
}

ptime validate_block::current_time() const
{
    return second_clock::universal_time();
}

bool validate_block::is_valid_time_stamp(uint32_t timestamp) const
{
    const auto two_hour_future = current_time() + hours(2);
    const auto block_time = from_time_t(timestamp);
    return block_time <= two_hour_future;
}

bool validate_block::is_valid_proof_of_work(hash_digest hash, uint32_t bits)
{
    hash_number target;
    if (!target.set_compact(bits))
        return false;

    if (target <= 0 || target > max_target())
        return false;

    hash_number our_value;
    our_value.set_hash(hash);
    return (our_value <= target);
}

// Determine if code is in the op_n range.
inline bool within_op_n(chain::opcode code)
{
    const auto value = static_cast<uint8_t>(code);
    constexpr auto op_1 = static_cast<uint8_t>(chain::opcode::op_1);
    constexpr auto op_16 = static_cast<uint8_t>(chain::opcode::op_16);
    return op_1 <= value && value <= op_16;
}

// Return the op_n index (i.e. value of n).
inline uint8_t decode_op_n(chain::opcode code)
{
    BITCOIN_ASSERT(within_op_n(code));
    const auto value = static_cast<uint8_t>(code);
    constexpr auto op_0 = static_cast<uint8_t>(chain::opcode::op_1) - 1;
    return value - op_0;
}

inline size_t count_script_sigops(
    const chain::operation::stack& operations, bool accurate)
{
    size_t total_sigs = 0;
    chain::opcode last_opcode = chain::opcode::bad_operation;
    for (const auto& op: operations)
    {
        if (op.code == chain::opcode::checksig ||
            op.code == chain::opcode::checksigverify)
        {
            total_sigs++;
        }
        else if (
            op.code == chain::opcode::checkmultisig ||
            op.code == chain::opcode::checkmultisigverify)
        {
            if (accurate && within_op_n(last_opcode))
                total_sigs += decode_op_n(last_opcode);
            else
                total_sigs += 20;
        }

        last_opcode = op.code;
    }

    return total_sigs;
}

size_t validate_block::legacy_sigops_count(const chain::transaction& tx)
{
    size_t total_sigs = 0;
    for (const auto& input: tx.inputs)
    {
        const auto& operations = input.script.operations;
        total_sigs += count_script_sigops(operations, false);
    }

    for (const auto& output: tx.outputs)
    {
        const auto& operations = output.script.operations;
        total_sigs += count_script_sigops(operations, false);
    }

    return total_sigs;
}

size_t validate_block::legacy_sigops_count(const chain::transaction::list& txs)
{
    size_t total_sigs = 0;
    for (const auto& tx: txs)
        total_sigs += legacy_sigops_count(tx);

    return total_sigs;
}

code validate_block::accept_block() const
{
    const auto& header = current_block_.header;
    if (header.bits != work_required(testnet_))
        return error::incorrect_proof_of_work;

    RETURN_IF_STOPPED();

    if (header.timestamp <= median_time_past())
        return error::timestamp_too_early;

    RETURN_IF_STOPPED();

    // Txs should be final when included in a block.
    for (const auto& tx: current_block_.transactions)
    {
        if (!tx.is_final(height_, header.timestamp))
            return error::non_final_transaction;

        RETURN_IF_STOPPED();
    }

    // Ensure that the block passes checkpoints.
    // This is both DOS protection and performance optimization for sync.
    const auto block_hash = header.hash();
    if (!checkpoint::validate(block_hash, height_, checkpoints_))
        return error::checkpoints_failed;

    RETURN_IF_STOPPED();

    // Reject version=1 blocks after switchover point.
    if (header.version < 2 && height_ > max_version1_height)
        return error::old_version_block;

    RETURN_IF_STOPPED();

    // Enforce version=2 rule that coinbase starts with serialized height.
    if (header.version >= 2 &&
        !is_valid_coinbase_height(height_, current_block_))
        return error::coinbase_height_mismatch;

    return error::success;
}

uint32_t validate_block::work_required(bool is_testnet) const
{
    if (height_ == 0)
        return max_work_bits;

    const auto is_retarget_height = [](size_t height)
    {
        return height % retargeting_interval == 0;
    };

    if (is_retarget_height(height_))
    {
        // This is the total time it took for the last 2016 blocks.
        const auto actual = actual_timespan(retargeting_interval);

        // Now constrain the time between an upper and lower bound.
        const auto constrained = range_constrain(actual,
            target_timespan / retargeting_factor,
            target_timespan * retargeting_factor);

        hash_number retarget;
        retarget.set_compact(previous_block_bits());
        retarget *= constrained;
        retarget /= target_timespan;
        if (retarget > max_target())
            retarget = max_target();

        return retarget.compact();
    }

    if (!is_testnet)
        return previous_block_bits();

    // Remainder is testnet in not-retargeting scenario.
    // ------------------------------------------------------------------------

    const auto max_time_gap = fetch_block(height_ - 1).timestamp + 
        2 * target_spacing;

    if (current_block_.header.timestamp > max_time_gap)
        return max_work_bits;

    const auto last_non_special_bits = [this, is_retarget_height]()
    {
        chain::header previous_block;
        auto previous_height = height_;

        // TODO: this is very suboptimal, cache the set of change points.
        // Loop backwards until we find a difficulty change point,
        // or we find a block which does not have max_bits (is not special).
        while (!is_retarget_height(previous_height))
        {
            --previous_height;

            // Test for non-special block.
            previous_block = fetch_block(previous_height);
            if (previous_block.bits != max_work_bits)
                break;
        }

        return previous_block.bits;
    };

    return last_non_special_bits();
}

bool validate_block::is_valid_coinbase_height(size_t height, 
    const chain::block& block)
{
    // There are old blocks with version incorrectly set to 2. Ignore them.
    if (height < max_version1_height)
        return true;

    // Checks whether the block height is in the coinbase tx input script.
    // Version 2 blocks and onwards.
    if (block.header.version < 2 || block.transactions.empty() || 
        block.transactions.front().inputs.empty())
        return false;

    // First get the serialized coinbase input script as a series of bytes.
    const auto& coinbase_tx = block.transactions.front();
    const auto& coinbase_script = coinbase_tx.inputs.front().script;
    const auto raw_coinbase = coinbase_script.to_data(false);

    // Try to recreate the expected bytes.
    chain::script expect_coinbase;
    script_number expect_number(height);
    expect_coinbase.operations.push_back(
        { chain::opcode::special, expect_number.data() });

    // Save the expected coinbase script.
    const auto expect = expect_coinbase.to_data(false);

    // Perform comparison of the first bytes with raw_coinbase.
    if (expect.size() > raw_coinbase.size())
        return false;

    return std::equal(expect.begin(), expect.end(), raw_coinbase.begin());
}

code validate_block::connect_block() const
{
    const auto& transactions = current_block_.transactions;

    // BIP30 security fix, and exclusion for two txs/blocks.
    if (!is_special_duplicate(current_block_, height_))
    {
        ////////////// TODO: parallelize. //////////////
        for (const auto& tx: transactions)
        {
            if (is_spent_duplicate(tx))
                return error::duplicate_or_spent;

            RETURN_IF_STOPPED();
        }
    }

    uint64_t fees = 0;
    size_t total_sigops = 0;
    const auto count = transactions.size();

    ////////////// TODO: parallelize. //////////////
    for (size_t tx_index = 0; tx_index < count; ++tx_index)
    {
        uint64_t value_in = 0;
        const auto& tx = transactions[tx_index];

        // It appears that this is also checked in check_block().
        total_sigops += legacy_sigops_count(tx);
        if (total_sigops > max_block_script_sig_operations)
            return error::too_many_sigs;

        RETURN_IF_STOPPED();

        // Count sigops for tx 0, but we don't perform
        // the other checks on coinbase tx.
        if (tx.is_coinbase())
            continue;

        RETURN_IF_STOPPED();

        // Consensus checks here.
        if (!validate_inputs(tx, tx_index, value_in, total_sigops))
            return error::validate_inputs_failed;

        RETURN_IF_STOPPED();

        if (!validate_transaction::tally_fees(tx, value_in, fees))
            return error::fees_out_of_range;
    }

    RETURN_IF_STOPPED();

    const auto& coinbase = transactions.front();
    const auto coinbase_value = coinbase.total_output_value();

    if (coinbase_value > block_value(height_) + fees)
        return error::coinbase_too_large;

    return error::success;
}

bool validate_block::is_spent_duplicate(const chain::transaction& tx) const
{
    const auto tx_hash = tx.hash();

    // Is there a matching previous tx?
    if (!transaction_exists(tx_hash))
        return false;

    // Are all outputs spent?
    ////////////// TODO: parallelize. //////////////
    for (uint32_t output_index = 0; output_index < tx.outputs.size();
        ++output_index)
    {
        if (!is_output_spent({ tx_hash, output_index }))
            return false;
    }

    return true;
}

bool validate_block::validate_inputs(const chain::transaction& tx,
    size_t index_in_parent, uint64_t& value_in, size_t& total_sigops) const
{
    BITCOIN_ASSERT(!tx.is_coinbase());

    ////////////// TODO: parallelize. //////////////
    for (size_t input_index = 0; input_index < tx.inputs.size(); ++input_index)
        if (!connect_input(index_in_parent, tx, input_index, value_in,
            total_sigops))
        {
            log::warning(LOG_VALIDATE) << "Invalid input ["
                << encode_hash(tx.hash()) << ":"
                << input_index << "]";
            return false;
        }

    return true;
}

bool script_hash_signature_operations_count(size_t& out_count,
    const chain::script& output_script, const chain::script& input_script)
{
    using namespace chain;
    constexpr auto strict = script::parse_mode::strict;

    if (input_script.operations.empty() ||
        output_script.pattern() != script_pattern::pay_script_hash)
    {
        out_count = 0;
        return true;
    }

    const auto& last_data = input_script.operations.back().data;
    script eval_script;
    if (!eval_script.from_data(last_data, false, strict))
    {
        return false;
    }

    out_count = count_script_sigops(eval_script.operations, true);
    return true;
}

bool validate_block::connect_input(size_t index_in_parent,
    const chain::transaction& current_tx, size_t input_index,
    uint64_t& value_in, size_t& total_sigops) const
{
    BITCOIN_ASSERT(input_index < current_tx.inputs.size());

    // Lookup previous output
    size_t previous_height;
    chain::transaction previous_tx;
    const auto& input = current_tx.inputs[input_index];
    const auto& previous_output = input.previous_output;

    // This searches the blockchain and then the orphan pool up to and
    // including the current (orphan) block and excluding blocks above fork.
    if (!fetch_transaction(previous_tx, previous_height, previous_output.hash))
    {
        log::warning(LOG_VALIDATE) << "Failure fetching input transaction.";
        return false;
    }

    const auto& previous_tx_out = previous_tx.outputs[previous_output.index];

    // Signature operations count if script_hash payment type.
    size_t count;
    if (!script_hash_signature_operations_count(count,
        previous_tx_out.script, input.script))
    {
        log::warning(LOG_VALIDATE) << "Invalid eval script.";
        return false;
    }

    total_sigops += count;
    if (total_sigops > max_block_script_sig_operations)
    {
        log::warning(LOG_VALIDATE) << "Total sigops exceeds block maximum.";
        return false;
    }

    // Get output amount
    const auto output_value = previous_tx_out.value;
    if (output_value > max_money())
    {
        log::warning(LOG_VALIDATE) << "Output money exceeds 21 million.";
        return false;
    }

    // Check coinbase maturity has been reached
    if (previous_tx.is_coinbase())
    {
        BITCOIN_ASSERT(previous_height <= height_);
        const auto height_difference = height_ - previous_height;
        if (height_difference < coinbase_maturity)
        {
            log::warning(LOG_VALIDATE) << "Immature coinbase spend attempt.";
            return false;
        }
    }

    if (!validate_transaction::validate_consensus(previous_tx_out.script,
        current_tx, input_index, current_block_.header, height_))
    {
        log::warning(LOG_VALIDATE) << "Input script invalid consensus.";
        return false;
    }

    // Search for double spends.
    if (is_output_spent(previous_output, index_in_parent, input_index))
    {
        log::warning(LOG_VALIDATE) << "Double spend attempt.";
        return false;
    }

    // Increase value_in by this output's value
    value_in += output_value;
    if (value_in > max_money())
    {
        log::warning(LOG_VALIDATE) << "Input money exceeds 21 million.";
        return false;
    }

    return true;
}

#undef RETURN_IF_STOPPED

} // namespace blockchain
} // namespace libbitcoin
