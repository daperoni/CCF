# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the Apache 2.0 License.
import infra.e2e_args
import infra.ccf
import infra.proc
import infra.remote
import json
import ledger
import coincurve
from coincurve._libsecp256k1 import ffi, lib
from coincurve.context import GLOBAL_CONTEXT
from enum import IntEnum
import sys
import os
import base64

import cryptography.x509
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.backends import default_backend

from loguru import logger as LOG

# As per mbedtls md_type_t
class SignedReqDigest(IntEnum):
    MD_NONE = 0
    MD_MD2 = 1
    MD_MD4 = 2
    MD_MD5 = 3
    MD_SHA1 = 4
    MD_SHA224 = 5
    MD_SHA256 = 6
    MD_SHA384 = 7
    MD_SHA512 = 8
    MD_RIPEMD160 = 9


# This function calls the native API and does not rely on the
# imported library's implementation. Though not being used by
# the current test, it might still be helpful to have this
# sequence of native calls for verification, in case the
# imported library's code changes.
def verify_recover_secp256k1_bc_native(
    signature, req, hasher=coincurve.utils.sha256, context=GLOBAL_CONTEXT
):
    # Compact
    native_rec_sig = ffi.new("secp256k1_ecdsa_recoverable_signature *")
    raw_sig, recovery_id = signature[:64], coincurve.utils.bytes_to_int(signature[64:])
    lib.secp256k1_ecdsa_recoverable_signature_parse_compact(
        context.ctx, native_rec_sig, raw_sig, recovery_id
    )

    # Recover public key
    native_public_key = ffi.new("secp256k1_pubkey *")
    msg_hash = hasher(req) if hasher is not None else req
    lib.secp256k1_ecdsa_recover(
        context.ctx, native_public_key, native_rec_sig, msg_hash
    )

    # Convert
    native_standard_sig = ffi.new("secp256k1_ecdsa_signature *")
    lib.secp256k1_ecdsa_recoverable_signature_convert(
        context.ctx, native_standard_sig, native_rec_sig
    )

    # Verify
    ret = lib.secp256k1_ecdsa_verify(
        context.ctx, native_standard_sig, msg_hash, native_public_key
    )


def verify_recover_secp256k1_bc(
    signature, req, hasher=coincurve.utils.sha256, context=GLOBAL_CONTEXT
):
    msg_hash = hasher(req) if hasher is not None else req
    rec_sig = coincurve.ecdsa.deserialize_recoverable(signature)
    public_key = coincurve.PublicKey(coincurve.ecdsa.recover(req, rec_sig))
    n_sig = coincurve.ecdsa.recoverable_convert(rec_sig)

    if not lib.secp256k1_ecdsa_verify(
        context.ctx, n_sig, msg_hash, public_key.public_key
    ):
        raise RuntimeError("Failed to verify SECP256K1 bitcoin signature")


def verify_sig(raw_cert, sig, req, request_body, md):
    try:
        cert = cryptography.x509.load_der_x509_certificate(
            raw_cert, backend=default_backend()
        )

        digest = (
            hashes.SHA256()
            if md == SignedReqDigest.MD_SHA256
            else cert.signature_hash_algorithm
        )

        # verify that the digest matches the hash of the body
        h = hashes.Hash(digest, backend=default_backend())
        h.update(request_body)
        raw_req_digest = h.finalize()
        header_digest = base64.b64decode(req.decode().split("SHA-256=")[1])
        assert (
            header_digest == raw_req_digest
        ), "Digest header does not match request body"

        pub_key = cert.public_key()
        hash_alg = ec.ECDSA(digest)
        pub_key.verify(sig, req, hash_alg)
    except cryptography.exceptions.InvalidSignature as e:
        # we support a non-standard curve, which is also being
        # used for bitcoin.
        if pub_key._curve.name != "secp256k1":
            raise e

        verify_recover_secp256k1_bc(sig, req)


def run(args):
    hosts = ["localhost", "localhost"]

    ledger_filename = None

    with infra.ccf.network(
        hosts, args.binary_dir, args.debug_nodes, args.perf_nodes, pdb=args.pdb
    ) as network:
        network.start_and_join(args)
        primary, term = network.find_primary()

        LOG.debug("Propose to add a new member (with a different curve)")
        infra.proc.ccall(
            network.key_generator,
            f"--name=member4",
            "--gen-key-share",
            f"--curve={infra.ccf.ParticipantsCurve.secp256k1.name}",
        )
        result, error = network.consortium.propose_add_member(
            1, primary, "member4_cert.pem", "member4_kshare_pub.pem"
        )

        # When proposal is added the proposal id and the result of running
        # complete proposal are returned
        assert not result["completed"]
        proposal_id = result["id"]

        # 2 out of 3 members vote to accept the new member so that
        # that member can send its own proposals
        LOG.debug("2/3 members accept the proposal")
        result = network.consortium.vote(1, primary, proposal_id, True)
        assert result[0] and not result[1]

        LOG.debug("Failed vote as unsigned")
        result = network.consortium.vote(2, primary, proposal_id, True, True)
        assert (
            not result[0]
            and result[1]["code"] == infra.jsonrpc.ErrorCode.RPC_NOT_SIGNED.value
        )

        result = network.consortium.vote(2, primary, proposal_id, True)
        assert result[0] and result[1]

        ledger_filename = network.find_primary()[0].remote.ledger_path()

    LOG.debug("Audit the ledger file for member votes")
    l = ledger.Ledger(ledger_filename)

    # this maps a member_id to a cert object, and is updated when we iterate the transactions,
    # so that we always have the correct cert for a member on a given transaction
    members = {}
    verified_votes = 0

    for tr in l:
        tables = tr.get_public_domain().get_tables()
        members_table = tables["ccf.member_certs"]
        for cert, member_id in members_table.items():
            members[member_id] = cert

        if "ccf.voting_history" in tables:
            votinghistory_table = tables["ccf.voting_history"]
            for member_id, signed_request in votinghistory_table.items():
                # if the signed vote is stored - there has to be a member at this point
                assert member_id in members
                cert = members[member_id]
                sig = signed_request[0][0]
                req = signed_request[0][1]
                request_body = signed_request[0][2]
                digest = signed_request[0][3]
                verify_sig(cert, sig, req, request_body, digest)
                verified_votes += 1

    assert verified_votes >= 2


if __name__ == "__main__":

    args = infra.e2e_args.cli_args()
    args.package = "liblogging"
    run(args)
