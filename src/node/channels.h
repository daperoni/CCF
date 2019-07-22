// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "crypto/symmkey.h"
#include "ds/logger.h"
#include "entities.h"
#include "tls/keyexchange.h"
#include "tls/keypair.h"

#include <iostream>
#include <map>
#include <mbedtls/ecdh.h>

namespace ccf
{
  using SeqNo = uint64_t;
  using GcmHdr = crypto::GcmHeader<sizeof(SeqNo)>;

  enum ChannelStatus
  {
    INITIATED = 0,
    ESTABLISHED
  };

  class Channel
  {
  private:
    // Used for key exchange
    tls::KeyExchangeContext ctx;
    ChannelStatus status;

    // Used for AES GCM authentication/encryption
    std::unique_ptr<crypto::KeyAesGcm> key;
    std::atomic<SeqNo> seqNo{0};

  public:
    Channel() : status(INITIATED) {}

    std::optional<std::vector<uint8_t>> get_public()
    {
      if (status == ESTABLISHED)
        return {};

      return ctx.get_own_public();
    }

    void set_status(ChannelStatus status_)
    {
      status = status_;
    }

    ChannelStatus get_status()
    {
      return status;
    }

    bool load_peer_public(const uint8_t* bytes, size_t size)
    {
      if (status == ESTABLISHED)
        return false;

      ctx.load_peer_public(bytes, size);
      return true;
    }

    void establish()
    {
      auto shared_secret = ctx.compute_shared_secret();
      key = std::make_unique<crypto::KeyAesGcm>(shared_secret);
      ctx.free_ctx();
      status = ESTABLISHED;
    }

    void free_ctx()
    {
      if (status != ESTABLISHED)
        return;

      ctx.free_ctx();
    }

    void tag(GcmHdr& header, CBuffer aad)
    {
      if (status != ESTABLISHED)
        throw std::logic_error("Channel is not established for tagging");

      header.setIvSeq(seqNo.fetch_add(1));
      key->encrypt(header.getIv(), nullb, aad, nullptr, header.tag);
    }

    bool verify(const GcmHdr& header, CBuffer aad)
    {
      if (status != ESTABLISHED)
        throw std::logic_error("Channel is not established for verifying");

      return key->decrypt(header.getIv(), header.tag, nullb, aad, nullptr);
    }

    void encrypt(GcmHdr& header, CBuffer aad, CBuffer plain, Buffer cipher)
    {
      if (status != ESTABLISHED)
        throw std::logic_error("Channel is not established for encrypting");

      header.setIvSeq(seqNo.fetch_add(1));
      key->encrypt(header.getIv(), plain, aad, cipher.p, header.tag);
    }

    bool decrypt(
      const GcmHdr& header, CBuffer aad, CBuffer cipher, Buffer plain)
    {
      if (status != ESTABLISHED)
        throw std::logic_error("Channel is not established for decrypting");

      return key->decrypt(header.getIv(), header.tag, cipher, aad, plain.p);
    }
  };

  class ChannelManager
  {
  private:
    std::unordered_map<NodeId, std::unique_ptr<Channel>> channels;
    tls::KeyPairPtr network_kp;

  public:
    ChannelManager(const std::vector<uint8_t>& network_pkey) :
      network_kp(tls::make_key_pair(network_pkey))
    {}

    Channel& get(NodeId peer_id)
    {
      auto search = channels.find(peer_id);
      if (search != channels.end())
      {
        return *search->second;
      }

      auto channel = std::make_unique<Channel>();
      channels.emplace(peer_id, std::move(channel));
      return *channels[peer_id];
    }

    std::optional<std::vector<uint8_t>> get_signed_public(NodeId peer_id)
    {
      const auto own_public_for_peer_ = get(peer_id).get_public();
      if (!own_public_for_peer_.has_value())
        return {};

      const auto& own_public_for_peer = own_public_for_peer_.value();

      auto signature = network_kp->sign(own_public_for_peer);

      // Serialise channel public and network signature
      // Length-prefix both
      auto space =
        own_public_for_peer.size() + signature.size() + 2 * sizeof(size_t);
      std::vector<uint8_t> ret(space);
      auto data_ = ret.data();
      serialized::write(data_, space, own_public_for_peer.size());
      serialized::write(
        data_, space, own_public_for_peer.data(), own_public_for_peer.size());
      serialized::write(data_, space, signature.size());
      serialized::write(data_, space, signature.data(), signature.size());

      return ret;
    }

    bool load_peer_signed_public(
      NodeId peer_id, const std::vector<uint8_t>& peer_signed_public)
    {
      auto& channel = get(peer_id);

      // Verify signature
      auto network_pubk = tls::make_public_key(network_kp->public_key());

      auto data = peer_signed_public.data();
      auto data_remaining = peer_signed_public.size();

      auto peer_public_size = serialized::read<size_t>(data, data_remaining);
      auto peer_public_start = data;

      if (peer_public_size > data_remaining)
      {
        LOG_FAIL_FMT(
          "Peer public key header wants {} bytes, but only {} remain",
          peer_public_size,
          data_remaining);
        return false;
      }

      data += peer_public_size;
      data_remaining -= peer_public_size;

      auto signature_size = serialized::read<size_t>(data, data_remaining);
      auto signature_start = data;

      if (signature_size > data_remaining)
      {
        LOG_FAIL_FMT(
          "Signature header wants {} bytes, but only {} remain",
          signature_size,
          data_remaining);
        return false;
      }

      if (signature_size < data_remaining)
      {
        LOG_FAIL_FMT(
          "Expected signature to use all remaining {} bytes, but only uses {}",
          data_remaining,
          signature_size);
        return false;
      }

      if (!network_pubk->verify(
            peer_public_start,
            peer_public_size,
            signature_start,
            signature_size))
      {
        LOG_FAIL_FMT(
          "node2node peer signature verification failed {}", peer_id);
        return false;
      }

      // Load peer public
      if (!channel.load_peer_public(peer_public_start, peer_public_size))
      {
        return false;
      }

      // Channel can be established
      channel.establish();

      LOG_INFO_FMT("node2node channel with {} is now established", peer_id);

      return true;
    }
  };
}
