/*
 * Copyright (c) 2018-2021 mcumgr authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** SMP - Simple Management Protocol. */

#include <assert.h>
#include <string.h>

#include "tinycbor/cbor.h"
#include "mgmt/endian.h"
#include "mgmt/mgmt.h"
#include "smp/smp.h"

/**
 * Converts a request opcode to its corresponding response opcode.
 */
static uint8_t
smp_rsp_op(uint8_t req_op)
{
	if (req_op == MGMT_OP_READ) {
		return MGMT_OP_READ_RSP;
	} else {
		return MGMT_OP_WRITE_RSP;
	}
}

static void
smp_init_rsp_hdr(const struct mgmt_hdr *req_hdr, struct mgmt_hdr *rsp_hdr)
{
	*rsp_hdr = (struct mgmt_hdr) {
		.nh_len = 0,
		.nh_flags = 0,
		.nh_op = smp_rsp_op(req_hdr->nh_op),
		.nh_group = req_hdr->nh_group,
		.nh_seq = req_hdr->nh_seq,
		.nh_id = req_hdr->nh_id,
	};
}

static int
smp_read_hdr(struct smp_streamer *streamer, struct mgmt_hdr *dst_hdr)
{
	struct cbor_decoder_reader *reader;

	reader = streamer->mgmt_stmr.reader;

	if (reader->message_size < sizeof(*dst_hdr)) {
		return MGMT_ERR_EINVAL;
	}

	reader->cpy(reader, (char *)dst_hdr, 0, sizeof(*dst_hdr));
	return 0;
}

static int
smp_write_hdr(struct smp_streamer *streamer, const struct mgmt_hdr *src_hdr)
{
	int rc;

	rc = mgmt_streamer_write_at(&streamer->mgmt_stmr, 0, src_hdr, sizeof(*src_hdr));
	return mgmt_err_from_cbor(rc);
}

static int
smp_build_err_rsp(struct smp_streamer *streamer,
				  const struct mgmt_hdr *req_hdr,
				  int status)
{
	struct CborEncoder map;
	struct mgmt_ctxt cbuf;
	struct mgmt_hdr rsp_hdr;
	int rc;

	rc = mgmt_ctxt_init(&cbuf, &streamer->mgmt_stmr);
	if (rc != 0) {
		return rc;
	}

	smp_init_rsp_hdr(req_hdr, &rsp_hdr);
	rc = smp_write_hdr(streamer, &rsp_hdr);
	if (rc != 0) {
		return rc;
	}

	rc = cbor_encoder_create_map(&cbuf.encoder, &map, CborIndefiniteLength);
	if (rc != 0) {
		return rc;
	}

	rc = mgmt_write_rsp_status(&cbuf, status);
	if (rc != 0) {
		return rc;
	}

	rc = cbor_encoder_close_container(&cbuf.encoder, &map);
	if (rc != 0) {
		return rc;
	}

	rsp_hdr.nh_len = cbor_encode_bytes_written(&cbuf.encoder) - MGMT_HDR_SIZE;
	mgmt_hton_hdr(&rsp_hdr);
	rc = smp_write_hdr(streamer, &rsp_hdr);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

/**
 * Processes a single SMP request and generates a response payload (i.e.,
 * everything after the management header).  On success, the response payload
 * is written to the supplied cbuf but not transmitted.  On failure, no error
 * response gets written; the caller is expected to build an error response
 * from the return code.
 *
 * @param cbuf		A cbuf containing the request and response buffer.
 * @param req_hdr	The management header belonging to the incoming request (host-byte order).
 *
 * @return A MGMT_ERR_[...] error code.
 */
static int
smp_handle_single_payload(struct mgmt_ctxt *cbuf, const struct mgmt_hdr *req_hdr,
			  bool *handler_found)
{
	const struct mgmt_handler *handler;
	mgmt_handler_fn handler_fn;
	struct CborEncoder payload_encoder;
	int rc;

	handler = mgmt_find_handler(req_hdr->nh_group, req_hdr->nh_id);
	if (handler == NULL) {
		return MGMT_ERR_ENOTSUP;
	}

	/* Begin response payload.  Response fields are inserted into the root
	 * map as key value pairs.
	 */
	rc = cbor_encoder_create_map(&cbuf->encoder, &payload_encoder, CborIndefiniteLength);
	rc = mgmt_err_from_cbor(rc);
	if (rc != 0) {
		return rc;
	}

	switch (req_hdr->nh_op) {
	case MGMT_OP_READ:
		handler_fn = handler->mh_read;
		break;

	case MGMT_OP_WRITE:
		handler_fn = handler->mh_write;
		break;

	default:
		return MGMT_ERR_EINVAL;
	}

	if (handler_fn) {
		*handler_found = true;
		mgmt_evt(MGMT_EVT_OP_CMD_RECV, req_hdr->nh_group, req_hdr->nh_id, NULL);

		rc = handler_fn(cbuf);
	} else {
		rc = MGMT_ERR_ENOTSUP;
	}

	if (rc != 0) {
		return rc;
	}

	/* End response payload. */
	rc = cbor_encoder_close_container(&cbuf->encoder, &payload_encoder);
	return mgmt_err_from_cbor(rc);
}

/**
 * Processes a single SMP request and generates a complete response (i.e.,
 * header and payload).  On success, the response is written using the supplied
 * streamer but not transmitted.  On failure, no error response gets written;
 * the caller is expected to build an error response from the return code.
 *
 * @param streamer	The SMP streamer to use for reading the request and writing the response.
 * @param req_hdr	The management header belonging to the incoming request (host-byte order).
 *
 * @return A MGMT_ERR_[...] error code.
 */
static int
smp_handle_single_req(struct smp_streamer *streamer, const struct mgmt_hdr *req_hdr,
		      bool *handler_found)
{
	struct mgmt_ctxt cbuf;
	struct mgmt_hdr rsp_hdr;
	int rc;

	rc = mgmt_ctxt_init(&cbuf, &streamer->mgmt_stmr);
	if (rc != 0) {
		return rc;
	}

	/* Write a dummy header to the beginning of the response buffer.  Some
	 * fields will need to be fixed up later.
	 */
	smp_init_rsp_hdr(req_hdr, &rsp_hdr);
	rc = smp_write_hdr(streamer, &rsp_hdr);
	if (rc != 0) {
		return rc;
	}

	/* Process the request and write the response payload. */
	rc = smp_handle_single_payload(&cbuf, req_hdr, handler_found);
	if (rc != 0) {
		return rc;
	}

	/* Fix up the response header with the correct length. */
	rsp_hdr.nh_len = cbor_encode_bytes_written(&cbuf.encoder) - MGMT_HDR_SIZE;
	mgmt_hton_hdr(&rsp_hdr);
	rc = smp_write_hdr(streamer, &rsp_hdr);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

/**
 * Attempts to transmit an SMP error response.  This function consumes both
 * supplied buffers.
 *
 * @param streamer	The SMP streamer for building and transmitting the response.
 * @param req_hdr	The header of the request which elicited the error.
 * @param req		The buffer holding the request.
 * @param rsp		The buffer holding the response, or NULL if none was allocated.
 * @param status	The status to indicate in the error response.
 */
static void
smp_on_err(struct smp_streamer *streamer, const struct mgmt_hdr *req_hdr,
		   void *req, void *rsp, int status)
{
	int rc;

	/* Prefer the response buffer for holding the error response.  If no
	 * response buffer was allocated, use the request buffer instead.
	 */
	if (rsp == NULL) {
		rsp = req;
		req = NULL;
	}

	/* Clear the partial response from the buffer, if any. */
	mgmt_streamer_reset_buf(&streamer->mgmt_stmr, rsp);
	mgmt_streamer_init_writer(&streamer->mgmt_stmr, rsp);

	/* Build and transmit the error response. */
	rc = smp_build_err_rsp(streamer, req_hdr, status);
	if (rc == 0) {
		streamer->tx_rsp_cb(streamer, rsp, streamer->mgmt_stmr.cb_arg);
		rsp = NULL;
	}

	/* Free any extra buffers. */
	mgmt_streamer_free_buf(&streamer->mgmt_stmr, req);
	mgmt_streamer_free_buf(&streamer->mgmt_stmr, rsp);
}

/**
 * Processes all SMP requests in an incoming packet.  Requests are processed
 * sequentially from the start of the packet to the end.  Each response is sent
 * individually in its own packet.  If a request elicits an error response,
 * processing of the packet is aborted.  This function consumes the supplied
 * request buffer regardless of the outcome.
 *
 * @param streamer	The streamer to use for reading, writing, and transmitting.
 * @param req		A buffer containing the request packet.
 *
 * @return 0 on success, MGMT_ERR_ECORRUPT if buffer starts with non SMP data header
 *                       or there is not enough bytes to process header,
 *                       or other MGMT_ERR_[...] code on failure.
 */
int
smp_process_request_packet(struct smp_streamer *streamer, void *req)
{
	struct mgmt_hdr req_hdr;
	struct mgmt_evt_op_cmd_done_arg cmd_done_arg;
	void *rsp;
	bool valid_hdr, handler_found;
	int rc = 0;

	rsp = NULL;

	mgmt_streamer_init_reader(&streamer->mgmt_stmr, req);

	while (streamer->mgmt_stmr.reader->message_size > 0) {
		handler_found = false;
		valid_hdr = false;

		/* Read the management header and strip it from the request. */
		rc = smp_read_hdr(streamer, &req_hdr);
		if (rc != 0) {
			rc = MGMT_ERR_ECORRUPT;
			break;
		} else {
			valid_hdr = true;
		}
		mgmt_ntoh_hdr(&req_hdr);
		/* Does buffer contain whole message? */
		if (streamer->mgmt_stmr.reader->message_size < (req_hdr.nh_len + MGMT_HDR_SIZE)) {
			rc = MGMT_ERR_ECORRUPT;
			break;
		}

		mgmt_streamer_trim_front(&streamer->mgmt_stmr, req, MGMT_HDR_SIZE);

		rsp = mgmt_streamer_alloc_rsp(&streamer->mgmt_stmr, req);
		if (rsp == NULL) {
			rc = MGMT_ERR_ENOMEM;
			break;
		}

		rc = mgmt_streamer_init_writer(&streamer->mgmt_stmr, rsp);
		if (rc != 0) {
			break;
		}

		/* Process the request payload and build the response. */
		rc = smp_handle_single_req(streamer, &req_hdr, &handler_found);
		if (rc != 0) {
			break;
		}

		/* Send the response. */
		rc = streamer->tx_rsp_cb(streamer, rsp, streamer->mgmt_stmr.cb_arg);
		rsp = NULL;
		if (rc != 0) {
			break;
		}

		/* Trim processed request to free up space for subsequent responses. */
		mgmt_streamer_trim_front(&streamer->mgmt_stmr, req, req_hdr.nh_len);

		cmd_done_arg.err = MGMT_ERR_EOK;
		mgmt_evt(MGMT_EVT_OP_CMD_DONE, req_hdr.nh_group, req_hdr.nh_id,
				 &cmd_done_arg);
	}

	if (rc != 0 && valid_hdr) {
		smp_on_err(streamer, &req_hdr, req, rsp, rc);

		if (handler_found) {
			cmd_done_arg.err = rc;
			mgmt_evt(MGMT_EVT_OP_CMD_DONE, req_hdr.nh_group, req_hdr.nh_id,
				 &cmd_done_arg);
		}

		return rc;
	}

	mgmt_streamer_free_buf(&streamer->mgmt_stmr, req);
	mgmt_streamer_free_buf(&streamer->mgmt_stmr, rsp);
	return rc;
}
