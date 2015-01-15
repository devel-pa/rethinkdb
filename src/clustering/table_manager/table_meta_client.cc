// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/table_manager/table_meta_client.hpp"

#include "clustering/generic/raft_core.tcc"
#include "concurrency/cross_thread_signal.hpp"

table_meta_client_t::table_meta_client_t(
        mailbox_manager_t *_mailbox_manager,
        watchable_map_t<peer_id_t, table_meta_manager_bcard_t>
            *_table_meta_manager_directory,
        watchable_map_t<std::pair<peer_id_t, namespace_id_t>, table_meta_bcard_t>
            *_table_meta_directory) :
    mailbox_manager(_mailbox_manager),
    table_meta_manager_directory(_table_meta_manager_directory),
    table_meta_directory(_table_meta_directory),
    table_metadata_by_id(&table_metadata_by_id_var),
    table_meta_directory_subs(table_meta_directory,
        std::bind(&table_meta_client_t::on_directory_change, this, ph::_1, ph::_2),
        true)
    { }

bool table_meta_client_t::find(
        const database_id_t &database,
        const name_string_t &name,
        namespace_id_t *table_id_out,
        size_t *count_out) {
    *count_out = 0;
    table_metadata_by_id.get_watchable()->read_all(
        [&](const namespace_id_t &key, const table_metadata_t *value) {
            if (value->database == database && value->name == name) {
                ++*count_out;
                *table_id_out = key;
            }
        });
    return (*count_out == 1);
}

bool table_meta_client_t::get_name(
        const namespace_id_t &table_id,
        database_id_t *db_out,
        name_string_t *name_out) {
    bool ok;
    table_metadata_by_id.get_watchable()->read_key(table_id,
        [&](const table_metadata_t *metadata) {
            if (metadata == nullptr) {
                ok = false;
            } else {
                ok = true;
                *db_out = metadata->database;
                *name_out = metadata->name;
            }
        });
    return ok;
}

void table_meta_client_t::list_names(
        std::map<namespace_id_t, std::pair<database_id_t, name_string_t> > *names_out) {
    table_metadata_by_id.get_watchable()->read_all(
        [&](const namespace_id_t &table_id, const table_metadata_t *metadata) {
            (*names_out)[table_id] = std::make_pair(metadata->database, metadata->name);
        });
}

bool table_meta_client_t::get_config(
        const namespace_id_t &table_id,
        signal_t *interruptor_on_caller,
        table_config_and_shards_t *config_out) {
    cross_thread_signal_t interruptor(interruptor_on_caller, home_thread());
    on_thread_t thread_switcher(home_thread());

    /* Find a mailbox of a server that claims to be hosting the given table */
    table_meta_manager_bcard_t::get_config_mailbox_t::address_t best_mailbox;
    table_meta_manager_bcard_t::timestamp_t best_timestamp;
    table_meta_directory->read_all(
    [&](const std::pair<peer_id_t, namespace_id_t> &key,
            const table_meta_bcard_t *table_bcard) {
        if (key.second == table_id) {
            table_meta_manager_directory->read_key(key.first,
            [&](const table_meta_manager_bcard_t *server_bcard) {
                if (server_bcard != nullptr) {
                    if (best_mailbox.is_nil() ||
                            table_bcard->timestamp.supersedes(best_timestamp)) {
                        best_mailbox = server_bcard->get_config_mailbox;
                        best_timestamp = table_bcard->timestamp;
                    }
                }
            });
        }
    });
    if (best_mailbox.is_nil()) {
        return false;
    }

    /* Send a request to the server we found */
    disconnect_watcher_t dw(mailbox_manager, best_mailbox.get_peer());
    promise_t<std::map<namespace_id_t, table_config_and_shards_t> > promise;
    mailbox_t<void(std::map<namespace_id_t, table_config_and_shards_t>)> ack_mailbox(
        mailbox_manager,
        [&](signal_t *,
                const std::map<namespace_id_t, table_config_and_shards_t> &configs) {
            promise.pulse(configs);
        });
    send(mailbox_manager, best_mailbox,
        boost::make_optional(table_id), ack_mailbox.get_address());
    wait_any_t done_cond(promise.get_ready_signal(), &dw);
    wait_interruptible(&done_cond, &interruptor);
    std::map<namespace_id_t, table_config_and_shards_t> maybe_result = promise.wait();
    if (maybe_result.empty()) {
        return false;
    }
    guarantee(maybe_result.size() == 1);
    *config_out = maybe_result.at(table_id);
    return true;
}

void table_meta_client_t::list_configs(
        signal_t *interruptor_on_caller,
        std::map<namespace_id_t, table_config_and_shards_t> *configs_out) {
    cross_thread_signal_t interruptor(interruptor_on_caller, home_thread());
    on_thread_t thread_switcher(home_thread());
    configs_out->clear();

    /* Collect mailbox addresses for every single server we can see */
    std::vector<table_meta_manager_bcard_t::get_config_mailbox_t::address_t>
        addresses;
    table_meta_manager_directory->read_all(
    [&](const peer_id_t &, const table_meta_manager_bcard_t *server_bcard) {
        addresses.push_back(server_bcard->get_config_mailbox);
    });

    /* Send a message to every server and collect all of the results */
    pmap(addresses.begin(), addresses.end(),
    [&](const table_meta_manager_bcard_t::get_config_mailbox_t::address_t &a) {
        disconnect_watcher_t dw(mailbox_manager, a.get_peer());
        promise_t<std::map<namespace_id_t, table_config_and_shards_t> > promise;
        mailbox_t<void(std::map<namespace_id_t, table_config_and_shards_t>)> ack_mailbox(
        mailbox_manager,
        [&](signal_t *,
                const std::map<namespace_id_t, table_config_and_shards_t> &configs) {
            promise.pulse(configs);
        });
        send(mailbox_manager, a,
            boost::optional<namespace_id_t>(), ack_mailbox.get_address());
        wait_any_t done_cond(promise.get_ready_signal(), &dw, &interruptor);
        done_cond.wait_lazily_unordered();
        if (promise.get_ready_signal()->is_pulsed()) {
            std::map<namespace_id_t, table_config_and_shards_t> maybe_result =
                promise.wait();
            configs_out->insert(maybe_result.begin(), maybe_result.end());
        }
    });

    /* The `pmap()` above will abort early without throwing anything if the interruptor
    is pulsed, so we have to throw the exception here */
    if (interruptor.is_pulsed()) {
        throw interrupted_exc_t();
    }
}

table_meta_client_t::result_t table_meta_client_t::create(
        const table_config_and_shards_t &initial_config,
        signal_t *interruptor_on_caller,
        namespace_id_t *table_id_out) {
    cross_thread_signal_t interruptor(interruptor_on_caller, home_thread());
    on_thread_t thread_switcher(home_thread());

    *table_id_out = generate_uuid();

    /* Prepare the message that we'll be sending to each server */
    table_meta_manager_bcard_t::timestamp_t timestamp;
    timestamp.epoch.timestamp = current_microtime();
    timestamp.epoch.id = generate_uuid();
    timestamp.log_index = 0;
    std::set<server_id_t> servers;
    for (const table_config_t::shard_t &shard : initial_config.config.shards) {
        servers.insert(shard.replicas.begin(), shard.replicas.end());
    }
    table_raft_state_t raft_state;
    raft_state.config = initial_config;
    raft_config_t raft_config;
    for (const server_id_t &server_id : servers) {
        raft_member_id_t member_id = generate_uuid();
        raft_state.member_ids[server_id] = member_id;
        raft_config.voting_members.insert(member_id);
    }
    raft_persistent_state_t<table_raft_state_t> raft_ps =
        raft_persistent_state_t<table_raft_state_t>::make_initial(
            raft_state, raft_config);

    /* Find the business cards of the servers we'll be sending to */
    std::map<server_id_t, table_meta_manager_bcard_t> bcards;
    table_meta_manager_directory->read_all(
        [&](const peer_id_t &, const table_meta_manager_bcard_t *bc) {
            if (servers.count(bc->server_id) == 1) {
                bcards[bc->server_id] = *bc;
            }
        });

    size_t num_acked = 0;
    pmap(bcards.begin(), bcards.end(),
    [&](const std::pair<server_id_t, table_meta_manager_bcard_t> &pair) {
        try {
            /* Send the message for the server and wait for a reply */
            disconnect_watcher_t dw(mailbox_manager,
                pair.second.action_mailbox.get_peer());
            cond_t got_ack;
            mailbox_t<void()> ack_mailbox(mailbox_manager,
                [&](signal_t *) { got_ack.pulse(); });
            send(mailbox_manager, pair.second.action_mailbox,
                *table_id_out,
                timestamp,
                false,
                boost::optional<raft_member_id_t>(raft_state.member_ids.at(pair.first)),
                boost::optional<raft_persistent_state_t<table_raft_state_t> >(raft_ps),
                ack_mailbox.get_address());
            wait_any_t interruptor_combined(&dw, &interruptor);
            wait_interruptible(&got_ack, &interruptor_combined);

            ++num_acked;
        } catch (const interrupted_exc_t &) {
            /* do nothing */
        }
    });
    if (interruptor.is_pulsed()) {
        throw interrupted_exc_t();
    }

    if (num_acked > 0) {
        /* Wait until the table appears in the directory. It may never appear in the
        directory if it's deleted or we lose contact immediately after the table is
        created; this is why we have a timeout. */
        signal_timer_t timeout;
        timeout.start(10*1000);
        wait_any_t interruptor_combined(&interruptor, &timeout);
        try {
            table_metadata_by_id_var.run_key_until_satisfied(*table_id_out,
                [](const table_metadata_t *m) { return m != nullptr; },
                &interruptor_combined);
        } catch (const interrupted_exc_t &) {
            if (interruptor.is_pulsed()) {
                throw;
            } else {
                return result_t::maybe;
            }
        }
        table_metadata_by_id.flush();
        return result_t::success;
    } else if (!bcards.empty()) {
        return result_t::maybe;
    } else {
        return result_t::failure;
    }
}

table_meta_client_t::result_t table_meta_client_t::drop(
        const namespace_id_t &table_id,
        signal_t *interruptor_on_caller) {
    cross_thread_signal_t interruptor(interruptor_on_caller, home_thread());
    on_thread_t thread_switcher(home_thread());

    /* Construct a special timestamp that supersedes all regular timestamps */
    table_meta_manager_bcard_t::timestamp_t drop_timestamp;
    drop_timestamp.epoch.timestamp = std::numeric_limits<microtime_t>::max();
    drop_timestamp.epoch.id = nil_uuid();
    drop_timestamp.log_index = std::numeric_limits<raft_log_index_t>::max();

    /* Find all servers that are hosting the table */
    std::map<server_id_t, table_meta_manager_bcard_t> bcards;
    table_meta_directory->read_all(
    [&](const std::pair<peer_id_t, namespace_id_t> &key,
            const table_meta_bcard_t *) {
        if (key.second == table_id) {
            table_meta_manager_directory->read_key(key.first,
            [&](const table_meta_manager_bcard_t *bc) {
                if (bc != nullptr) {
                    bcards[bc->server_id] = *bc;
                }
            });
        }
    });

    /* Send a message to each server. It's possible that the table will move to other
    servers while the messages are in-flight; but this is OK, since the servers will pass
    the deletion message on. */
    size_t num_acked = 0;
    pmap(bcards.begin(), bcards.end(),
    [&](const std::pair<server_id_t, table_meta_manager_bcard_t> &pair) {
        try {
            disconnect_watcher_t dw(mailbox_manager,
                pair.second.action_mailbox.get_peer());
            cond_t got_ack;
            mailbox_t<void()> ack_mailbox(mailbox_manager,
                [&](signal_t *) { got_ack.pulse(); });
            send(mailbox_manager, pair.second.action_mailbox,
                table_id, drop_timestamp, true, boost::optional<raft_member_id_t>(),
                boost::optional<raft_persistent_state_t<table_raft_state_t> >(),
                ack_mailbox.get_address());
            wait_any_t interruptor_combined(&dw, &interruptor);
            wait_interruptible(&got_ack, &interruptor_combined);
            ++num_acked;
        } catch (const interrupted_exc_t &) {
            /* do nothing */
        }
    });
    if (interruptor.is_pulsed()) {
        throw interrupted_exc_t();
    }

    if (num_acked > 0) {
        /* Wait until the table disappears from the directory. */
        signal_timer_t timeout;
        timeout.start(10*1000);
        wait_any_t interruptor_combined(&interruptor, &timeout);
        try {
            table_metadata_by_id_var.run_key_until_satisfied(table_id,
                [](const table_metadata_t *m) { return m == nullptr; },
                &interruptor_combined);
        } catch (const interrupted_exc_t &) {
            if (interruptor.is_pulsed()) {
                throw;
            } else {
                return result_t::maybe;
            }
        }
        table_metadata_by_id.flush();
        return result_t::success;
    } else if (!bcards.empty()) {
        return result_t::maybe;
    } else {
        return result_t::failure;
    }
}

table_meta_client_t::result_t table_meta_client_t::set_config(
        const namespace_id_t &table_id,
        const table_config_and_shards_t &new_config,
        signal_t *interruptor_on_caller) {
    cross_thread_signal_t interruptor(interruptor_on_caller, home_thread());
    on_thread_t thread_switcher(home_thread());

    /* Find the server (if any) which is acting as leader for the table */
    table_meta_manager_bcard_t::set_config_mailbox_t::address_t best_mailbox;
    table_meta_manager_bcard_t::timestamp_t best_timestamp;
    table_meta_directory->read_all(
    [&](const std::pair<peer_id_t, namespace_id_t> &key,
            const table_meta_bcard_t *table_bcard) {
        if (key.second == table_id) {
            table_meta_manager_directory->read_key(key.first,
            [&](const table_meta_manager_bcard_t *server_bcard) {
                if (server_bcard != nullptr && table_bcard->is_leader) {
                    if (best_mailbox.is_nil() ||
                            table_bcard->timestamp.supersedes(best_timestamp)) {
                        best_mailbox = server_bcard->set_config_mailbox;
                        best_timestamp = table_bcard->timestamp;
                    }
                }
            });
        }
    });
    if (best_mailbox.is_nil()) {
        return result_t::failure;
    }

    /* Send a message to the server and wait for a reply */
    disconnect_watcher_t dw(mailbox_manager, best_mailbox.get_peer());
    promise_t<boost::optional<table_meta_manager_bcard_t::timestamp_t> >
        promise;
    mailbox_t<void(boost::optional<table_meta_manager_bcard_t::timestamp_t>)>
        ack_mailbox(mailbox_manager,
        [&](signal_t *,
                boost::optional<table_meta_manager_bcard_t::timestamp_t> res) {
            promise.pulse(res);
        });
    send(mailbox_manager, best_mailbox, table_id, new_config, ack_mailbox.get_address());
    wait_any_t done_cond(promise.get_ready_signal(), &dw);
    wait_interruptible(&done_cond, &interruptor);
    if (dw.is_pulsed()) {
        return result_t::maybe;
    }

    /* Sometimes the server will reply by indicating that something went wrong */
    boost::optional<table_meta_manager_bcard_t::timestamp_t> timestamp = promise.wait();
    if (!static_cast<bool>(timestamp)) {
        return result_t::maybe;
    }

    /* We know for sure that the change has been applied; now we just need to wait until
    the change is visible in the directory before returning. The naive thing is to wait
    until the table's name and database match whatever we just changed the config to. But
    this could go wrong if the table's name and database are changed again in quick
    succession. We detect that other case by using the timestamps. */
    signal_timer_t timeout;
    timeout.start(10*1000);
    wait_any_t interruptor_combined(&interruptor, &timeout);
    try {
        table_metadata_by_id_var.run_key_until_satisfied(table_id,
            [&](const table_metadata_t *m) {
                return m == nullptr || m->timestamp.supersedes(*timestamp) ||
                    (m->name == new_config.config.name &&
                        m->database == new_config.config.database);
            },
            &interruptor_combined);
    } catch (const interrupted_exc_t &) {
        if (interruptor.is_pulsed()) {
            throw;
        }
    }

    table_metadata_by_id.flush();
    return result_t::success;
}

void table_meta_client_t::on_directory_change(
        const std::pair<peer_id_t, namespace_id_t> &key,
        const table_meta_bcard_t *dir_value) {
    table_metadata_by_id_var.change_key(key.second,
    [&](bool *md_exists, table_metadata_t *md_value) -> bool {
        if (dir_value != nullptr && !*md_exists) {
            *md_exists = true;
            md_value->witnesses = std::set<peer_id_t>({key.first});
            md_value->database = dir_value->database;
            md_value->name = dir_value->name;
            md_value->primary_key = dir_value->primary_key;
            md_value->timestamp = dir_value->timestamp;
        } else if (dir_value != nullptr && *md_exists) {
            md_value->witnesses.insert(key.first);
            if (dir_value->timestamp.supersedes(md_value->timestamp)) {
                md_value->database = dir_value->database;
                md_value->name = dir_value->name;
                md_value->timestamp = dir_value->timestamp;
            }
        } else {
            if (*md_exists) {
                md_value->witnesses.erase(key.first);
                if (md_value->witnesses.empty()) {
                    *md_exists = false;
                }
            }
        }
        return true;
    });
}

