// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "consensus/ledgerenclave.h"
#include "consensus/pbft/libbyz/Big_req_table.h"
#include "consensus/pbft/libbyz/Client_proxy.h"
#include "consensus/pbft/libbyz/Status.h"
#include "consensus/pbft/libbyz/libbyz.h"
#include "consensus/pbft/libbyz/network.h"
#include "consensus/pbft/libbyz/receive_message_base.h"
#include "consensus/pbft/pbftconfig.h"
#include "consensus/pbft/pbfttypes.h"
#include "ds/logger.h"
#include "enclave/rpcmap.h"
#include "enclave/rpcsessions.h"
#include "host/ledger.h"
#include "kv/kvtypes.h"
#include "node/nodetypes.h"
#include "node/rpc/jsonrpc.h"

#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pbft
{
  class PbftEnclaveNetwork : public INetwork
  {
  public:
    PbftEnclaveNetwork(
      pbft::NodeId id, std::shared_ptr<ccf::NodeToNode> n2n_channels) :
      n2n_channels(n2n_channels),
      id(id)
    {}

    virtual ~PbftEnclaveNetwork() = default;

    bool Initialize(in_port_t port) override
    {
      return true;
    }

    void set_receiver(IMessageReceiveBase* receiver)
    {
      message_receiver_base = receiver;
    }

    int Send(Message* msg, IPrincipal& principal) override
    {
      NodeId to = principal.pid();
      if (to == id)
      {
        // If a replica sends a message to itself (e.g. if f == 0), handle
        // the message straight away without writing it to the ringbuffer
        message_receiver_base->receive_message(
          (const uint8_t*)(msg->contents()), msg->size());
        return msg->size();
      }

      PbftHeader hdr = {PbftMsgType::pbft_message, id};

      // TODO: Encrypt msg here
      std::vector<uint8_t> serialized_msg(sizeof(PbftHeader) + msg->size());
      auto data_ = serialized_msg.data();
      auto space = serialized_msg.size();
      serialized::write<PbftHeader>(data_, space, hdr);
      serialized::write(
        data_,
        space,
        reinterpret_cast<const uint8_t*>(msg->contents()),
        msg->size());

      n2n_channels->send_authenticated(
        ccf::NodeMsgType::consensus_msg, to, serialized_msg);
      // if (msg->tag() == 7)
      // {
      //   Status* status;
      //   Status::convert(msg, status);

      //   AppendEntries ae = {pbft_append_entries, id, 0, 0};
      //   n2n_channels->send_authenticated(
      //     ccf::NodeMsgType::consensus_msg, to, ae);
      //   return msg->size();
      // }
      return msg->size();
    }

    virtual Message* GetNextMessage() override
    {
      assert("Should not be called");
      return nullptr;
    }

    virtual bool has_messages(long to) override
    {
      return false;
    }

  private:
    std::shared_ptr<ccf::NodeToNode> n2n_channels;
    IMessageReceiveBase* message_receiver_base = nullptr;
    NodeId id;
  };

  template <class LedgerProxy, class ChannelProxy>
  class Pbft : public kv::Consensus
  {
  private:
    std::shared_ptr<ChannelProxy> channels;
    IMessageReceiveBase* message_receiver_base = nullptr;
    char* mem;
    std::unique_ptr<PbftEnclaveNetwork> pbft_network;
    std::unique_ptr<AbstractPbftConfig> pbft_config;
    std::unique_ptr<ClientProxy<kv::TxHistory::RequestID, void>> client_proxy;
    std::shared_ptr<enclave::RPCSessions> rpcsessions;
    SeqNo global_commit_seqno;
    View last_commit_view;
    std::unique_ptr<pbft::PbftStore> store;
    std::unique_ptr<consensus::LedgerEnclave> ledger;
    Index append_entries_index = 0;

    // When this is set, only public domain is deserialised when receving append
    // entries
    bool public_only = false;

    struct view_change_info
    {
      view_change_info(View view_, SeqNo min_global_commit_) :
        min_global_commit(min_global_commit_),
        view(view_)
      {}

      SeqNo min_global_commit;
      View view;
    };

    std::vector<view_change_info> view_change_list;

    struct register_global_commit_info
    {
      pbft::PbftStore* store;
      SeqNo* global_commit_seqno;
      View* last_commit_view;
      std::vector<view_change_info>* view_change_list;
    } register_global_commit_ctx;

  public:
    Pbft(
      std::unique_ptr<pbft::PbftStore> store_,
      std::shared_ptr<ChannelProxy> channels_,
      NodeId id,
      size_t sig_max_tx,
      std::unique_ptr<consensus::LedgerEnclave> ledger_,
      std::shared_ptr<enclave::RPCMap> rpc_map,
      std::shared_ptr<enclave::RPCSessions> rpcsessions_,
      pbft::RequestsMap& pbft_requests_map,
      pbft::PrePreparesMap& pbft_pre_prepares_map,
      const std::string& privk_pem,
      const std::vector<uint8_t>& cert) :
      Consensus(id),
      channels(channels_),
      rpcsessions(rpcsessions_),
      ledger(std::move(ledger_)),
      global_commit_seqno(1),
      last_commit_view(0),
      store(std::move(store_)),
      view_change_list(1, view_change_info(0, 0))
    {
      // configure replica
      GeneralInfo general_info;
      general_info.num_replicas = 1;
      general_info.num_clients = 1;
      general_info.max_faulty = 0;
      general_info.service_name = "generic";
      general_info.auth_timeout = 1800000;
      general_info.view_timeout = 5000;
      general_info.status_timeout = 100;
      general_info.recovery_timeout = 9999250000;
      general_info.max_requests_between_signatures =
        sig_max_tx / Max_requests_in_batch;

      // Adding myself
      PrincipalInfo my_info;
      my_info.id = local_id;
      my_info.port = 0;
      my_info.ip = "256.256.256.256"; // Invalid
      my_info.cert = cert;
      my_info.host_name = "machineB";
      my_info.is_replica = true;

      ::NodeInfo node_info = {my_info, privk_pem, general_info};

      int mem_size = 40 * 8192;
      mem = (char*)malloc(mem_size);
      bzero(mem, mem_size);

      pbft_network = std::make_unique<PbftEnclaveNetwork>(local_id, channels);
      pbft_config = std::make_unique<PbftConfigCcf>(rpc_map);

      auto used_bytes = Byz_init_replica(
        node_info,
        mem,
        mem_size,
        pbft_config->get_exec_command(),
        0,
        0,
        pbft_network.get(),
        pbft_requests_map,
        pbft_pre_prepares_map,
        *store,
        &message_receiver_base);
      LOG_INFO_FMT("PBFT setup for local_id: {}", local_id);

      pbft_config->set_service_mem(mem + used_bytes);
      pbft_config->set_receiver(message_receiver_base);
      pbft_network->set_receiver(message_receiver_base);

      Byz_start_replica();

      LOG_INFO_FMT("PBFT setting up client proxy");
      client_proxy =
        std::make_unique<ClientProxy<kv::TxHistory::RequestID, void>>(
          *message_receiver_base, 5000, 10000);

      auto reply_handler_cb = [](Reply* m, void* ctx) {
        auto cp =
          static_cast<ClientProxy<kv::TxHistory::RequestID, void>*>(ctx);
        cp->recv_reply(m);
      };
      message_receiver_base->register_reply_handler(
        reply_handler_cb, client_proxy.get());

      auto global_commit_cb = [](kv::Version version, ::View view, void* ctx) {
        auto p = static_cast<register_global_commit_info*>(ctx);
        if (version == kv::NoVersion || version < *p->global_commit_seqno)
        {
          return;
        }
        *p->global_commit_seqno = version;

        if (*p->last_commit_view < view)
        {
          p->view_change_list->emplace_back(view, version);
        }
        p->store->compact(version);
      };

      register_global_commit_ctx.store = store.get();
      register_global_commit_ctx.global_commit_seqno = &global_commit_seqno;
      register_global_commit_ctx.last_commit_view = &last_commit_view;
      register_global_commit_ctx.view_change_list = &view_change_list;

      message_receiver_base->register_global_commit(
        global_commit_cb, &register_global_commit_ctx);
    }

    bool on_request(const kv::TxHistory::RequestCallbackArgs& args) override
    {
      pbft::Request request = {
        args.actor, args.caller_id, args.caller_cert, args.request, {}};
      auto serialized_req = request.serialise();

      auto rep_cb = [&](
                      void* owner,
                      kv::TxHistory::RequestID caller_rid,
                      int status,
                      uint8_t* reply,
                      size_t len) {
        LOG_DEBUG_FMT("PBFT reply callback for {}", caller_rid);

        return rpcsessions->reply_async(
          std::get<1>(caller_rid), {reply, reply + len});
      };

      LOG_DEBUG_FMT("PBFT sending request {}", args.rid);
      return client_proxy->send_request(
        args.rid,
        serialized_req.data(),
        serialized_req.size(),
        rep_cb,
        client_proxy.get());
    }

    View get_view() override
    {
      return message_receiver_base->view() + 2;
    }

    View get_view(SeqNo seqno) override
    {
      for (auto rit = view_change_list.rbegin(); rit != view_change_list.rend();
           ++rit)
      {
        view_change_info& info = *rit;
        if (info.min_global_commit <= seqno)
        {
          return info.view + 2;
        }
      }
      throw std::logic_error("should never be here");
    }

    SeqNo get_commit_seqno() override
    {
      return global_commit_seqno;
    }

    kv::NodeId primary() override
    {
      return message_receiver_base->primary();
    }

    bool is_primary() override
    {
      return message_receiver_base->is_primary();
    }

    bool is_backup() override
    {
      return !message_receiver_base->is_primary();
    }

    void add_configuration(
      SeqNo seqno,
      std::unordered_set<kv::NodeId> config,
      const NodeConf& node_conf) override
    {
      if (node_conf.node_id == local_id)
      {
        return;
      }

      PrincipalInfo info;
      info.id = node_conf.node_id;
      info.port = short(atoi(node_conf.port.c_str()));
      info.ip = "256.256.256.256"; // Invalid
      info.cert = node_conf.cert;
      info.host_name = node_conf.host_name;
      info.is_replica = true;
      Byz_add_principal(info);
      LOG_INFO_FMT("PBFT added node, id: {}", info.id);
    }

    void periodic(std::chrono::milliseconds elapsed) override
    {
      ITimer::handle_timeouts(elapsed);
    }

    template <typename T>
    size_t write_to_ledger(const T& data)
    {
      ledger->put_entry(data->data(), data->size());
      return data->size();
    }

    template <>
    size_t write_to_ledger<std::vector<uint8_t>>(
      const std::vector<uint8_t>& data)
    {
      ledger->put_entry(data);
      return data.size();
    }

    bool replicate(const kv::BatchDetachedBuffer& entries) override
    {
      for (auto& [index, data, globally_committable] : entries)
      {
        append_entries_index++;
        write_to_ledger(data);
      }
      return true;
    }

    bool replicate(const kv::BatchVector& entries) override
    {
      for (auto& [index, data, globally_committable] : entries)
      {
        append_entries_index++;
        write_to_ledger(data);
      }
      return true;
    }

    void recv_message(const uint8_t* data, size_t size) override
    {
      switch (serialized::peek<PbftMsgType>(data, size))
      {
        case pbft_message:
        {
          serialized::skip(data, size, sizeof(PbftHeader));
          message_receiver_base->receive_message(data, size);
          break;
        }
        case pbft_append_entries:
        {
          AppendEntries r;

          try
          {
            r =
              channels->template recv_authenticated<AppendEntries>(data, size);
          }
          catch (const std::logic_error& err)
          {
            LOG_FAIL_FMT(err.what());
            return;
          }

          for (Index i = r.prev_idx + 1; i <= r.idx; i++)
          {
            LOG_INFO_FMT(
              "RECORDING ENTRY FOR INDEX {} FOR DATA WITH SIZE {}", i, size);
            // if (i <= last_idx)
            // {
            //   // If the current entry has already been deserialised, skip the
            //   // payload for that entry
            //   ledger->skip_entry(data, size);
            //   continue;
            // }
            Index last_idx = i;
            // TODO record entry here?
            auto ret = ledger->record_entry(data, size);

            if (!ret.second)
            {
              // NB: This will currently never be triggered.
              // This should only fail if there is malformed data. Truncate
              // the log and reply false.
              LOG_FAIL_FMT(
                "Recv append entries to {} from {} but the data is malformed",
                local_id,
                r.from_node);

              last_idx = r.prev_idx;
              ledger->truncate(r.prev_idx);
              return;
            }

            ccf::Store::Tx tx;
            auto deserialise_success = store->deserialise_views(
              ret.first, public_only, false, nullptr, &tx);

            switch (deserialise_success)
            {
              case kv::DeserialiseSuccess::FAILED:
              {
                throw std::logic_error(
                  "Replica failed to apply log entry " + std::to_string(i));
                break;
              }
              case kv::DeserialiseSuccess::PASS:
              {
                message_receiver_base->playback_transaction(tx);
                break;
              }
              case kv::DeserialiseSuccess::PASS_SIGNATURE:
              {
                throw std::logic_error(
                  "Received a history signature while running with PBFT!");
                break;
              }
            }
          }
          break;
        }
      }
    }

    void set_f(ccf::NodeId f) override
    {
      message_receiver_base->set_f(f);
    }

    void emit_signature() override
    {
      kv::Version version = store->current_version();
      if (message_receiver_base != nullptr)
      {
        message_receiver_base->emit_signature_on_next_pp(version);
      }
    }

    ConsensusType type() override
    {
      return ConsensusType::Pbft;
    }
  };
}