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
#ifndef LIBBITCOIN_BLOCKCHAIN_BLOCKCHAIN_IMPL_HPP
#define LIBBITCOIN_BLOCKCHAIN_BLOCKCHAIN_IMPL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>
#include <boost/interprocess/sync/file_lock.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/checkpoint.hpp>
#include <bitcoin/blockchain/define.hpp>
#include <bitcoin/blockchain/database.hpp>
#include <bitcoin/blockchain/implementation/organizer_impl.hpp>
#include <bitcoin/blockchain/implementation/simple_chain_impl.hpp>
#include <bitcoin/blockchain/organizer.hpp>
#include <bitcoin/blockchain/settings.hpp>

namespace libbitcoin {
namespace blockchain {

class BCB_API blockchain_impl
  : public block_chain
{
public:
    blockchain_impl(threadpool& pool, const settings& settings);

    // Non-copyable
    blockchain_impl(const blockchain_impl&) = delete;
    void operator=(const blockchain_impl&) = delete;

    void start(result_handler handler);
    void stop(result_handler handler);
    void stop();

    void store(const chain::block& block, store_block_handler handler);
    ////void import(const chain::block& block, block_import_handler handler);

    // fetch block header by height
    void fetch_block_header(uint64_t height,
        block_header_fetch_handler handler);

    // fetch block header by hash
    void fetch_block_header(const hash_digest& hash,
        block_header_fetch_handler handler);

    //// This should really be fetch_merkle_tree.
    ////void fetch_block_transaction_hashes(const hash_digest& hash,
    ////    transaction_hashes_fetch_handler handle_fetch);

    // fetch height of block by hash
    void fetch_block_height(const hash_digest& hash,
        block_height_fetch_handler handler);

    // fetch height of latest block
    void fetch_last_height(last_height_fetch_handler handler);

    // fetch transaction by hash
    void fetch_transaction(const hash_digest& hash,
        transaction_fetch_handler handler);

    // fetch height and offset within block of transaction by hash
    void fetch_transaction_index(const hash_digest& hash,
        transaction_index_fetch_handler handler);

    // fetch spend of an output point
    void fetch_spend(const chain::output_point& outpoint,
        spend_fetch_handler handler);

    // fetch outputs, values and spends for an address.
    void fetch_history(const wallet::payment_address& address,
        history_fetch_handler handler, const uint64_t limit=0,
        const uint64_t from_height=0);

    // fetch stealth results.
    void fetch_stealth(const binary_type& filter,
        stealth_fetch_handler handler, uint64_t from_height = 0);

    void subscribe_reorganize(reorganize_handler handler);

private:
    typedef std::atomic<size_t> sequential_lock;
    typedef std::function<bool(uint64_t)> perform_read_functor;

    void initialize_lock(const std::string& prefix);
    void start_write();

    template <typename Handler, typename... Args>
    void stop_write(Handler handler, Args&&... args)
    {
        ++slock_;

        // slock_ is now even again.
        BITCOIN_ASSERT(slock_ % 2 == 0);
        handler(std::forward<Args>(args)...);
    }

    void do_store(const chain::block& block, store_block_handler handle_store);

    // Uses sequential lock to try to read shared data.
    // Try to initiate asynchronous read operation. If it fails then
    // sleep for a small amount of time and then retry read operation.
    void fetch(perform_read_functor perform_read);

    template <typename Handler, typename... Args>
    bool finish_fetch(uint64_t slock, Handler handler, Args&&... args)
    {
        if (slock != slock_)
            return false;

        handler(std::forward<Args>(args)...);
        return true;
    }

    bool do_fetch_stealth(const binary_type& filter,
        stealth_fetch_handler handle_fetch, uint64_t from_height,
        uint64_t slock);

    bool stopped();

    // Queue for writes to the blockchain.
    dispatcher dispatch_;

    // Lock the database directory with a file lock.
    boost::interprocess::file_lock flock_;

    // sequential lock used for writes.
    sequential_lock slock_;
    bool stopped_;

    // Main database core.
    database::store store_;
    database database_;

    // Organize stuff
    orphan_pool orphans_;
    simple_chain_impl chain_;
    organizer_impl organizer_;
};

} // namespace blockchain
} // namespace libbitcoin

#endif

