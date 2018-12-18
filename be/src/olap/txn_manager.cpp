// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/storage_engine.h"

#include <signal.h>

#include <algorithm>
#include <cstdio>
#include <new>
#include <queue>
#include <set>
#include <random>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <rapidjson/document.h>
#include <thrift/protocol/TDebugProtocol.h>

#include "agent/file_downloader.h"
#include "olap/base_compaction.h"
#include "olap/cumulative_compaction.h"
#include "olap/lru_cache.h"
#include "olap/tablet_meta.h"
#include "olap/tablet_meta_manager.h"
#include "olap/push_handler.h"
#include "olap/reader.h"
#include "olap/schema_change.h"
#include "olap/data_dir.h"
#include "olap/utils.h"
#include "olap/data_writer.h"
#include "util/time.h"
#include "util/doris_metrics.h"
#include "util/pretty_printer.h"

using apache::thrift::ThriftDebugString;
using boost::filesystem::canonical;
using boost::filesystem::directory_iterator;
using boost::filesystem::path;
using boost::filesystem::recursive_directory_iterator;
using std::back_inserter;
using std::copy;
using std::inserter;
using std::list;
using std::map;
using std::nothrow;
using std::pair;
using std::priority_queue;
using std::set;
using std::set_difference;
using std::string;
using std::stringstream;
using std::vector;

namespace doris {

OLAPStatus TxnManager::add_txn(
    TPartitionId partition_id, TTransactionId transaction_id,
    TTabletId tablet_id, SchemaHash schema_hash, const PUniqueId& load_id) {

    pair<int64_t, int64_t> key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash);
    WriteLock wrlock(&_txn_map_lock);
    auto it = _transaction_tablet_map.find(key);
    if (it != _transaction_tablet_map.end()) {
        auto load_info = it->second.find(tablet_info);
        if (load_info != it->second.end()) {
            for (PUniqueId& pid : load_info->second) {
                if (pid.hi() == load_id.hi() && pid.lo() == load_id.lo()) {
                    LOG(WARNING) << "find transaction exists when add to engine."
                        << "partition_id: " << key.first
                        << ", transaction_id: " << key.second
                        << ", tablet: " << tablet_info.to_string();
                    return OLAP_ERR_PUSH_TRANSACTION_ALREADY_EXIST;
                }
            }
        }
    }

    _transaction_tablet_map[key][tablet_info].push_back(load_id);
    VLOG(3) << "add transaction to engine successfully."
            << "partition_id: " << key.first
            << ", transaction_id: " << key.second
            << ", tablet: " << tablet_info.to_string();
    return OLAP_SUCCESS;
}

OLAPStatus TxnManager::delete_txn(
    TPartitionId partition_id, TTransactionId transaction_id,
    TTabletId tablet_id, SchemaHash schema_hash) {

    pair<int64_t, int64_t> key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash);
    WriteLock wrlock(&_txn_map_lock);

    auto it = _transaction_tablet_map.find(key);
    if (it != _transaction_tablet_map.end()) {
        VLOG(3) << "delete transaction to engine successfully."
                << ",partition_id: " << key.first
                << ", transaction_id: " << key.second
                << ", tablet: " << tablet_info.to_string();
        it->second.erase(tablet_info);
        if (it->second.empty()) {
            _transaction_tablet_map.erase(it);
        }

        // delete transaction from tablet
        // delete from tablet is useless and it should be called at storageengine
        /*
        if (delete_from_tablet) {
            TabletSharedPtr tablet = get_tablet(tablet_info.tablet_id, tablet_info.schema_hash);
            if (tablet.get() != nullptr) {
                tablet->delete_pending_data(transaction_id);
            }
        }
        */
       return OLAP_SUCCESS;
    } else {
        return OLAP_ERR_TRANSACTION_NOT_EXIST;
    }
}

void TxnManager::get_tablet_related_txns(TabletSharedPtr tablet, int64_t* partition_id,
                                            std::set<int64_t>* transaction_ids) {
    if (tablet.get() == nullptr || partition_id == nullptr || transaction_ids == nullptr) {
        OLAP_LOG_WARNING("parameter is null when get transactions by tablet");
        return;
    }

    TabletInfo tablet_info(tablet->tablet_id(), tablet->schema_hash());
    ReadLock rdlock(&_txn_map_lock);
    for (auto& it : _transaction_tablet_map) {
        if (it.second.find(tablet_info) != it.second.end()) {
            *partition_id = it.first.first;
            transaction_ids->insert(it.first.second);
            VLOG(3) << "find transaction on tablet."
                    << "partition_id: " << it.first.first
                    << ", transaction_id: " << it.first.second
                    << ", tablet: " << tablet_info.to_string();
        }
    }
}

void TxnManager::get_txn_related_tablets(const TTransactionId transaction_id,
                                        TPartitionId partition_id,
                                        std::vector<TabletInfo>* tablet_infos) {
    // get tablets in this transaction
    pair<int64_t, int64_t> key(partition_id, transaction_id);

    _txn_map_lock.rdlock();
    auto it = _transaction_tablet_map.find(key);
    if (it == _transaction_tablet_map.end()) {
        OLAP_LOG_WARNING("could not find tablet for [partition_id=%ld transaction_id=%ld]",
                            partition_id, transaction_id);
        _txn_map_lock.unlock();
        return;
    }
    std::map<TabletInfo, std::vector<PUniqueId>> load_info_map = it->second;
    _txn_map_lock.unlock();

    // each tablet
    for (auto& load_info : load_info_map) {
        const TabletInfo& tablet_info = load_info.first;
        tablet_infos->push_back(tablet_info);
    }

}
                                

bool TxnManager::has_txn(TPartitionId partition_id, TTransactionId transaction_id,
                                 TTabletId tablet_id, SchemaHash schema_hash) {
    pair<int64_t, int64_t> key(partition_id, transaction_id);
    TabletInfo tablet_info(tablet_id, schema_hash);

    _txn_map_lock.rdlock();
    auto it = _transaction_tablet_map.find(key);
    bool found = it != _transaction_tablet_map.end()
                 && it->second.find(tablet_info) != it->second.end();
    _txn_map_lock.unlock();

    return found;
}

} // namespace doris