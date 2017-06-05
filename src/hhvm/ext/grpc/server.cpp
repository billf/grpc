/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "call.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "server.h"

#include <stdbool.h>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>

#include "completion_queue.h"
#include "server.h"
#include "channel.h"
#include "server_credentials.h"
#include "timeval.h"

#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/base/req-containers.h"
#include "hphp/runtime/vm/native-data.h"
#include "hphp/runtime/base/builtin-functions.h"

namespace HPHP {

Server::Server() {}
Server::~Server() { sweep(); }

void Server::init(grpc_server* server) {
  wrapped = server;
}

void Server::sweep() {
  if (wrapped) {
    grpc_server_shutdown_and_notify(wrapped, completion_queue, NULL);
    grpc_server_cancel_all_calls(wrapped);
    grpc_completion_queue_pluck(completion_queue, NULL,
                                gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
    grpc_server_destroy(wrapped);
    req::free(wrapped);
    wrapped = nullptr;
  }
}

grpc_server* Server::getWrapped() {
  return wrapped;
}

void HHVM_METHOD(Server, __construct,
  const Variant& args_array_or_null /* = null */) {
  auto server = Native::data<Server>(this_);
  if (args_array_or_null.isNull()) {
    server->init(grpc_server_create(NULL, NULL));
  } else {
    grpc_channel_args args;
    hhvm_grpc_read_args_array(args_array_or_null.toArray(), &args);
    server->init(grpc_server_create(&args, NULL));
    req::free(args.args);
  }

  grpc_server_register_completion_queue(server->getWrapped(), completion_queue, NULL);
}

Object HHVM_METHOD(Server, requestCall) {
  char *method_text;
  char *host_text;
  Object callObj;
  Call *call;
  Object timevalObj;
  Timeval *timeval;

  grpc_call_error error_code;
  grpc_call *call_;
  grpc_call_details details;
  grpc_metadata_array metadata;
  grpc_event event;
  Object resultObj = SystemLib::AllocStdClassObject();;

  auto server = Native::data<Server>(this_);

  grpc_call_details_init(&details);
  grpc_metadata_array_init(&metadata);
  error_code = grpc_server_request_call(server->getWrapped(), &call_, &details, &metadata,
                                 completion_queue, completion_queue, NULL);

  if (error_code != GRPC_CALL_OK) {
    throw_invalid_argument("request_call failed: %d", error_code);
    goto cleanup;
  }

  event = grpc_completion_queue_pluck(completion_queue, NULL,
                                        gpr_inf_future(GPR_CLOCK_REALTIME),
                                        NULL);

  if (!event.success) {
    throw_invalid_argument("Failed to request a call for some reason");
    goto cleanup;
  }

  method_text = grpc_slice_to_c_string(details.method);
  host_text = grpc_slice_to_c_string(details.host);

  resultObj.o_set("method_text", String(method_text));
  resultObj.o_set("host_text", String(host_text));

  gpr_free(method_text);
  gpr_free(host_text);

  callObj = create_object("Call", Array());
  call = Native::data<Call>(callObj);
  call->init(call_);

  timevalObj = create_object("Timeval", Array());
  timeval = Native::data<Timeval>(timevalObj);
  timeval->init(details.deadline);

  resultObj.o_set("call", callObj);
  resultObj.o_set("absolute_deadline", timevalObj);
  resultObj.o_set("metadata", grpc_parse_metadata_array(&metadata));

cleanup:
    grpc_call_details_destroy(&details);
    grpc_metadata_array_destroy(&metadata);
    return resultObj;
}

bool HHVM_METHOD(Server, addHttp2Port,
  const String& addr) {
  auto server = Native::data<Server>(this_);
  return (bool)grpc_server_add_insecure_http2_port(server->getWrapped(), addr.c_str());
}

bool HHVM_METHOD(Server, addSecureHttp2Port,
  const String& addr,
  const Object& server_credentials) {
  auto server = Native::data<Server>(this_);
  auto serverCredentials = Native::data<ServerCredentials>(server_credentials);
  return (bool)grpc_server_add_secure_http2_port(server->getWrapped(), addr.c_str(), serverCredentials->getWrapped());
}

void HHVM_METHOD(Server, start) {
  auto server = Native::data<Server>(this_);
  grpc_server_start(server->getWrapped());
}

} // namespace HPHP
