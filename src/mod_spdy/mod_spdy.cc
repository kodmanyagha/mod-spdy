// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// References to "TAMB" below refer to _The Apache Modules Book_ by Nick Kew
// (ISBN: 0-13-240967-4).

#include "httpd.h"
#include "http_connection.h"
#include "http_config.h"
#include "http_log.h"
#include "http_request.h"
#include "apr_optional.h"
#include "apr_optional_hooks.h"
#include "apr_tables.h"
#include "apr_thread_pool.h"

#include "mod_spdy/apache/apache_spdy_session_io.h"
#include "mod_spdy/apache/apache_spdy_stream_task_factory.h"
#include "mod_spdy/apache/apr_thread_pool_executor.h"
#include "mod_spdy/apache/config_commands.h"
#include "mod_spdy/apache/config_util.h"
#include "mod_spdy/apache/filters/http_to_spdy_filter.h"
#include "mod_spdy/apache/filters/spdy_to_http_filter.h"
#include "mod_spdy/apache/log_message_handler.h"
#include "mod_spdy/apache/pool_util.h"
#include "mod_spdy/common/connection_context.h"
#include "mod_spdy/common/spdy_server_config.h"
#include "mod_spdy/common/spdy_session.h"

extern "C" {
  // Declaring modified mod_ssl's optional hooks here (so that we don't need to
  // #include "mod_ssl.h").
  APR_DECLARE_OPTIONAL_FN(int, ssl_engine_disable, (conn_rec *));
  APR_DECLARE_OPTIONAL_FN(int, ssl_is_https, (conn_rec *));
  APR_DECLARE_EXTERNAL_HOOK(
      ssl, AP, int, npn_advertise_protos_hook,
      (conn_rec* connection, apr_array_header_t* protos));
  APR_DECLARE_EXTERNAL_HOOK(
      ssl, AP, int, npn_proto_negotiated_hook,
      (conn_rec* connection, char* proto_name, apr_size_t proto_name_len));
}

namespace {

// For now, we only support SPDY version 2.
// TODO(mdsteele): Pretty soon we will probably need to support SPDY v3.
const char* kSpdyProtocolName = "spdy/2";

// These global variables store the filter handles for our filters.  Normally,
// global variables would be very dangerous in a concurrent environment like
// Apache, but these ones are okay because they are assigned just once, at
// start-up (during which Apache is running single-threaded; see TAMB 2.2.1),
// and are read-only thereafter.
ap_filter_rec_t* gAntiChunkingFilterHandle = NULL;
ap_filter_rec_t* gHttpToSpdyFilterHandle = NULL;
ap_filter_rec_t* gSpdyToHttpFilterHandle = NULL;

// These global variables store pointers to "optional functions" defined in
// mod_ssl.  See TAMB 10.1.2 for more about optional functions.  These, too,
// are assigned just once, at start-up.
int (*gDisableSslForConnection)(conn_rec*) = NULL;
int (*gIsUsingSslForConnection)(conn_rec*) = NULL;

// A process-global thread pool for processing SPDY streams concurrently.  This
// is initialized once in *each child process* by our child-init hook.  Note
// that in a non-threaded MPM (e.g. Prefork), this thread pool will be used by
// just one SPDY connection at a time, but in a threaded MPM (e.g. Worker) it
// will shared by several SPDY connections at once.  That's okay though,
// because apr_thread_pool_t objects are thread-safe.  Users just have to make
// sure that they configure SpdyMaxThreadsPerProcess depending on the MPM.
apr_thread_pool* gPerProcessThreadPool = NULL;

// See TAMB 8.4.2
apr_status_t SpdyToHttpFilter(ap_filter_t* filter,
                              apr_bucket_brigade* brigade,
                              ap_input_mode_t mode,
                              apr_read_type_e block,
                              apr_off_t readbytes) {
  mod_spdy::SpdyToHttpFilter* spdy_to_http_filter =
      static_cast<mod_spdy::SpdyToHttpFilter*>(filter->ctx);
  return spdy_to_http_filter->Read(filter, brigade, mode, block, readbytes);
}

apr_status_t AntiChunkingFilter(ap_filter_t* filter,
                                apr_bucket_brigade* input_brigade) {
  // Make sure no one is already trying to chunk the data in this request.
  request_rec* request = filter->r;
  if (request->chunked != 0) {
    LOG(DFATAL) << "request->chunked == " << request->chunked
                << " in request " << request->the_request;
  }
  const char* transfer_encoding =
      apr_table_get(request->headers_out, "Transfer-Encoding");
  if (transfer_encoding != NULL) {
    LOG(DFATAL) << "transfer_encoding == \"" << transfer_encoding << "\""
                << " in request " << request->the_request;
  }

  // Setting the Transfer-Encoding header to "chunked" here will trick the core
  // HTTP_HEADER filter into not inserting the CHUNK filter.  We later remove
  // this header in our http-to-spdy filter.  It's a gross hack, but it seems
  // to work, and is much simpler than allowing the data to be chunked and then
  // having to de-chunk it ourselves.
  apr_table_setn(request->headers_out, "Transfer-Encoding", "chunked");

  // This filter only needs to run once, so now that it has run, remove it.
  ap_remove_output_filter(filter);
  return ap_pass_brigade(filter->next, input_brigade);
}

// See TAMB 8.4.1
apr_status_t HttpToSpdyFilter(ap_filter_t* filter,
                              apr_bucket_brigade* input_brigade) {
  // First, we need to do a couple things that are relevant to the details of
  // the anti-chunking filter.  We'll do them here rather than in the
  // HttpToSpdyFilter class so that we can see them right next to the
  // anti-chunking filter.

  // Make sure nothing unexpected has happened to the transfer encoding between
  // here and our anti-chunking filter.
  request_rec* request = filter->r;
  if (request->chunked != 0) {
    LOG(DFATAL) << "request->chunked == " << request->chunked
                << " in request " << request->the_request;
  }
  const char* transfer_encoding =
      apr_table_get(filter->r->headers_out, "Transfer-Encoding");
  if (transfer_encoding != NULL && strcmp(transfer_encoding, "chunked")) {
    LOG(DFATAL) << "transfer_encoding == \"" << transfer_encoding << "\""
                << " in request " << request->the_request;
  }
  // Remove the transfer-encoding header so that it does not appear in our SPDY
  // headers.
  apr_table_unset(request->headers_out, "Transfer-Encoding");

  // Okay, now that that's done, let's focus on translating HTTP to SPDY.
  mod_spdy::HttpToSpdyFilter* http_to_spdy_filter =
      static_cast<mod_spdy::HttpToSpdyFilter*>(filter->ctx);
  return http_to_spdy_filter->Write(filter, input_brigade);
}

// Called on server startup, after all modules have loaded.
void RetrieveOptionalFunctions() {
  gDisableSslForConnection = APR_RETRIEVE_OPTIONAL_FN(ssl_engine_disable);
  gIsUsingSslForConnection = APR_RETRIEVE_OPTIONAL_FN(ssl_is_https);
  // If mod_ssl isn't installed, we'll get back NULL for these functions.  Our
  // other hook functions will fail gracefully (i.e. do nothing) if these
  // functions are NULL, but if the user installed mod_spdy without mod_ssl and
  // expected it to do anything, we should warn them otherwise.
  if (gDisableSslForConnection == NULL &&
      gIsUsingSslForConnection == NULL) {
    LOG(WARNING) << "It seems that mod_spdy is installed but mod_ssl isn't.  "
                 << "Without SSL, the server cannot ever use SPDY.";
  }
  // Whether or not mod_ssl is installed, either both functions should be
  // non-NULL or both functions should be NULL.  Otherwise, something is wrong
  // (like, maybe some kind of bizarre mutant mod_ssl is installed) and
  // mod_spdy probably won't work correctly.
  if ((gDisableSslForConnection == NULL) ^
      (gIsUsingSslForConnection == NULL)) {
    LOG(DFATAL) << "Some, but not all, of mod_ssl's optional functions are "
                << "available.  What's going on?";
  }
}

// Called exactly once for each child process, before that process starts
// spawning worker threads.
void ChildInit(apr_pool_t* pool, server_rec* server) {
  const mod_spdy::SpdyServerConfig* config = mod_spdy::GetServerConfig(server);
  const int max_threads = config->max_threads_per_process();
  const apr_status_t status = apr_thread_pool_create(
      &gPerProcessThreadPool, max_threads, max_threads, pool);
  if (status != APR_SUCCESS) {
    ap_log_error(APLOG_MARK, APLOG_ALERT, status, server,
                 "Could not create mod_spdy thread pool; "
                 "mod_spdy will not function.");
  } else {
    // TODO(mdsteele): This is very strange.  If you _don't_ have this next
    // line (and we wouldn't expect to need it, having allocated the thread
    // pool in a memory pool), then Apache spits out a double-free error upon
    // exiting.  If you _do_ have this line, which instructs the memory pool to
    // destroy the thread pool during cleanup (shouldn't it be doing that
    // anyway?), then you _don't_ get a double-free error -- although Valgrind
    // will report that you may be leaking memory (not great, but probably okay
    // given that we're exiting anyway).  I don't know why this is.  Maybe
    // apr_thread_pool_t is buggy?  It seems possible, given that I can't seem
    // to find any project that actually uses them, so maybe they're not
    // well-tested.  Or maybe I'm just doing something wrong; but we should
    // probably find a replacement thread pool implementation.  Until then,
    // we'll keep this line around so that Apache doesn't spit stack traces at
    // us every time we exit.
    apr_pool_pre_cleanup_register(
        pool, gPerProcessThreadPool,
        reinterpret_cast<apr_status_t(*)(void*)>(apr_thread_pool_destroy));
  }
}

// A pre-connection hook, to be run _before_ mod_ssl's pre-connection hook.
// Disables mod_ssl for our slave connections.
int DisableSslForSlaves(conn_rec* connection, void* csd) {
  const mod_spdy::ConnectionContext* context =
      mod_spdy::GetConnectionContext(connection);

  // For master connections, the context object won't have been created yet (it
  // gets created in PreConnection).
  if (context == NULL) {
    return DECLINED;
  }

  // If the context has already been created, this must be a slave connection.
  DCHECK(context->is_slave());

  // Disable mod_ssl for the slave connection so it doesn't get in our way.
  if (gDisableSslForConnection == NULL ||
      gDisableSslForConnection(connection) == 0) {
    // We wouldn't have a slave connection unless mod_ssl were installed and
    // enabled on this server, so this outcome should be impossible.
    LOG(DFATAL) << "mod_ssl missing for slave connection";
  }
  return OK;
}

// A pre-connection hook, to be run _after_ mod_ssl's pre-connection hook, but
// just _before_ the core pre-connection hook.  For master connections, this
// checks if SSL is active; for slave connections, this adds our
// connection-level filters and prevents core filters from being inserted.
int PreConnection(conn_rec* connection, void* csd) {
  const mod_spdy::ConnectionContext* context =
      mod_spdy::GetConnectionContext(connection);

  // If the connection context has not yet been created, this is a "real"
  // connection (not one of our slave connections).
  if (context == NULL) {
    // Check if this connection is over SSL; if not, we definitely won't be
    // using SPDY.
    if (gIsUsingSslForConnection == NULL ||  // mod_ssl is not even loaded
        gIsUsingSslForConnection(connection) == 0) {
      // This is not an SSL connection, so we can't talk SPDY on it.
      return DECLINED;
    }

    // Okay, we've got a real connection over SSL, so we'll be negotiating with
    // the client to see if we can use SPDY for this connection.  Create our
    // connection context object to keep track of the negotiation.
    mod_spdy::CreateMasterConnectionContext(connection);
    return OK;
  }
  // If the context has already been created, this is a slave connection.
  else {
    DCHECK(context->is_slave());

    // Instantiate and add our SPDY-to-HTTP filter for the slave connection.
    // This is an Apache connection-level filter, so we add it here.  The
    // corresponding HTTP-to-SPDY filter is request-level, so we add that one
    // in InsertRequestFilters().
    mod_spdy::SpdyToHttpFilter* spdy_to_http_filter =
        new mod_spdy::SpdyToHttpFilter(context->slave_stream());
    mod_spdy::PoolRegisterDelete(connection->pool, spdy_to_http_filter);
    ap_add_input_filter_handle(
        gSpdyToHttpFilterHandle,  // filter handle
        spdy_to_http_filter,      // context (any void* we want)
        NULL,                     // request object
        connection);              // connection object

    // Prevent core pre-connection hooks from running (thus preventing core
    // filters from being inserted).
    return DONE;
  }
}

// Called to see if we want to take care of processing this connection -- if
// so, we do so and return OK, otherwise we return DECLINED.  For slave
// connections, we want to return DECLINED.  For "real" connections, we need to
// determine if they are using SPDY; if not we returned DECLINED, but if so we
// process this as a master SPDY connection and then return OK.
int ProcessConnection(conn_rec* connection) {
  // We do not want to attach to non-inbound connections (e.g. connections
  // created by mod_proxy).  Non-inbound connections do not get a scoreboard
  // hook, so we abort if the connection doesn't have the scoreboard hook.  See
  // http://mail-archives.apache.org/mod_mbox/httpd-dev/201008.mbox/%3C99EA83DCDE961346AFA9B5EC33FEC08B047FDC26@VF-MBX11.internal.vodafone.com%3E
  // for more details.
  if (connection->sbh == NULL) {
    return DECLINED;
  }

  // Our connection context object will have been created by now, unless our
  // pre-connection hook saw that this was a non-SSL connection, in which case
  // we won't be using SPDY so we can stop now.
  const mod_spdy::ConnectionContext* context =
      mod_spdy::GetConnectionContext(connection);
  if (context == NULL) {
    return DECLINED;
  }

  // If this is one of our slave connections (rather than a "real" connection),
  // then we don't want to deal with it here -- instead we will let Apache
  // treat it like a regular HTTP connection.
  if (context->is_slave()) {
    return DECLINED;
  }

  // In the unlikely event that we failed to create our per-process thread
  // pool, we're not going to be able to operate.
  if (gPerProcessThreadPool == NULL) {
    return DECLINED;
  }

  // We need to pull some data through mod_ssl in order to force the SSL
  // handshake, and hence NPN, to take place.  To that end, perform a small
  // SPECULATIVE read (and then throw away whatever data we got).
  apr_bucket_brigade* temp_brigade =
      apr_brigade_create(connection->pool, connection->bucket_alloc);
  const apr_status_t status =
      ap_get_brigade(connection->input_filters, temp_brigade,
                     AP_MODE_SPECULATIVE, APR_BLOCK_READ, 1);
  apr_brigade_destroy(temp_brigade);

  // If we were unable to pull any data through, give up.
  if (status != APR_SUCCESS) {
    // EOF errors are to be expected sometimes (e.g. if the connection was
    // closed).  If the error was something else, though, log an error.
    if (!APR_STATUS_IS_EOF(status)) {
      LOG(ERROR) << "Error during speculative read: " << status;
    }
    return DECLINED;
  }

  // If we did pull some data through, then NPN should have happened and our
  // OnNextProtocolNegotiated() hook should have been called by now.  If NPN
  // hasn't happened, it's probably because we're using an old version of
  // mod_ssl that doesn't support NPN, in which case we should probably warn
  // the user that mod_spdy isn't going to work.
  if (context->npn_state() == mod_spdy::ConnectionContext::NOT_DONE_YET) {
    LOG(WARNING) << "NPN didn't happen during SSL handshake.  Probably you're "
                 << "using an unpatched mod_ssl that doesn't support NPN.  "
                 << "Without NPN support, the server cannot ever use SPDY.";
  }
  // If NPN didn't choose SPDY, then don't use SPDY.
  if (context->npn_state() != mod_spdy::ConnectionContext::USING_SPDY) {
    return DECLINED;
  }

  // At this point, we and the client have agreed to use SPDY, so process this
  // as a SPDY master connection.
  mod_spdy::ApacheSpdySessionIO session_io(connection);
  mod_spdy::ApacheSpdyStreamTaskFactory task_factory(connection);
  mod_spdy::AprThreadPoolExecutor executor(gPerProcessThreadPool);
  mod_spdy::SpdySession spdy_session(
      mod_spdy::GetServerConfig(connection),
      &session_io, &task_factory, &executor);
  // This call will block until the session has closed down.
  spdy_session.Run();

  // Return OK to tell Apache that we handled this connection.
  return OK;
}

// Called by mod_ssl when it needs to decide what protocols to advertise to the
// client during Next Protocol Negotiation (NPN).
int AdvertiseNpnProtocols(conn_rec* connection, apr_array_header_t* protos) {
  // If the config file has disabled mod_spdy for this server, then we
  // shouldn't advertise SPDY to the client.
  if (!mod_spdy::GetServerConfig(connection)->spdy_enabled()) {
    return DECLINED;
  }

  // Advertise SPDY to the client.
  // TODO(mdsteele): Pretty soon we will probably need to support SPDY v3.  If
  //   we want to support both v2 and v3, we need to advertise both of them
  //   here; the one we prefer (presumably v3) should be pushed first.
  APR_ARRAY_PUSH(protos, const char*) = kSpdyProtocolName;
  return OK;
}

// Called by mod_ssl after Next Protocol Negotiation (NPN) has completed,
// informing us which protocol was chosen by the client.
int OnNextProtocolNegotiated(conn_rec* connection, char* proto_name,
                             apr_size_t proto_name_len) {
  mod_spdy::ConnectionContext* context =
      mod_spdy::GetConnectionContext(connection);

  // Our context object should have already been created in our pre-connection
  // hook, unless this is a non-SSL connection.  But if it's a non-SSL
  // connection, then NPN shouldn't be happening, and this hook shouldn't be
  // getting called!  So, let's LOG(DFATAL) if context is NULL here.
  if (context == NULL) {
    LOG(DFATAL) << "NPN happened, but there is no connection context.";
    return DECLINED;
  }

  // We disable mod_ssl for slave connections, so NPN shouldn't be happening
  // unless this is a non-slave connection.
  if (context->is_slave()) {
    LOG(DFATAL) << "mod_ssl was aparently not disabled for slave connection";
    return DECLINED;
  }

  // NPN should happen only once, so npn_state should still be NOT_DONE_YET.
  if (context->npn_state() != mod_spdy::ConnectionContext::NOT_DONE_YET) {
    LOG(DFATAL) << "NPN happened twice.";
    return DECLINED;
  }

  // If the client chose the SPDY version that we advertised, then mark this
  // connection as using SPDY.
  if (proto_name_len == strlen(kSpdyProtocolName) &&
      !strncmp(kSpdyProtocolName, proto_name, proto_name_len)) {
    context->set_npn_state(mod_spdy::ConnectionContext::USING_SPDY);
  }
  // Otherwise, explicitly mark this connection as not using SPDY.
  else {
    context->set_npn_state(mod_spdy::ConnectionContext::NOT_USING_SPDY);
  }
  return OK;
}

// Invoked once per HTTP request.  See http_request.h for details.
void InsertRequestFilters(request_rec* request) {
  conn_rec* connection = request->connection;
  const mod_spdy::ConnectionContext* context =
      mod_spdy::GetConnectionContext(connection);

  // Our context object should be present by now (having been created in our
  // pre-connection hook) unless this is a non-SSL connection, in which case we
  // definitely aren't using SPDY.
  if (context == NULL) {
    return;
  }

  // If this isn't one of our slave connections, don't insert any filters.
  if (!context->is_slave()) {
    return;
  }

  // Instantiate and add our HTTP-to-SPDY filter (and also our anti-chunking
  // filter) for the slave connection.  This is an Apache request-level filter,
  // so we add it here.  The corresponding SPDY-to-HTTP filter is
  // connection-level, so we add that one in PreConnection().
  mod_spdy::HttpToSpdyFilter* http_to_spdy_filter =
      new mod_spdy::HttpToSpdyFilter(context->slave_stream());
  PoolRegisterDelete(request->pool, http_to_spdy_filter);

  ap_add_output_filter_handle(
      gHttpToSpdyFilterHandle,    // filter handle
      http_to_spdy_filter,        // context (any void* we want)
      request,                    // request object
      connection);                // connection object

  ap_add_output_filter_handle(
      gAntiChunkingFilterHandle,  // filter handle
      NULL,                       // context (any void* we want)
      request,                    // request object
      connection);                // connection object
}

// Called when the module is loaded to register all of our hook functions.
void RegisterHooks(apr_pool_t* pool) {
  mod_spdy::InstallLogMessageHandler(pool);

  // Let users know that they are installing an experimental module.
  LOG(WARNING) << "mod_spdy is currently an experimental Apache module. "
               << "It is not yet suitable for production environments "
               << "and may have stability issues.";

  static const char* const modules_core[] = {"core.c", NULL};
  static const char* const modules_mod_ssl[] = {"mod_ssl.c", NULL};

  // Register a hook to be called after all modules have been loaded, so we can
  // retrieve optional functions from mod_ssl.
  ap_hook_optional_fn_retrieve(
      RetrieveOptionalFunctions,  // hook function to be called
      NULL,                       // predecessors
      NULL,                       // successors
      APR_HOOK_MIDDLE);           // position

  // Register a hook to be called once for each child process spawned by
  // Apache, before the MPM starts spawning worker threads.  We use this hook
  // to initialize our per-process thread pool.
  ap_hook_child_init(
      ChildInit,                  // hook function to be called
      NULL,                       // predecessors
      NULL,                       // successors
      APR_HOOK_MIDDLE);           // position

  // Register a pre-connection hook to turn off mod_ssl for our slave
  // connections.  This must run before mod_ssl's pre-connection hook, so that
  // we can disable mod_ssl before it inserts its filters, so we name mod_ssl
  // as an explicit successor.
  ap_hook_pre_connection(
      DisableSslForSlaves,        // hook function to be called
      NULL,                       // predecessors
      modules_mod_ssl,            // successors
      APR_HOOK_FIRST);            // position

  // Register our pre-connection hook, which will be called shortly before our
  // process-connection hook.  The hooking order is very important here.  In
  // particular:
  //   * We must run before the core pre-connection hook, so that we can return
  //     DONE and stop the core filters from being inserted.  Thus, we name
  //     core.c as a successor.
  //   * We should run after almost all other modules (except core.c) so that
  //     our returning DONE doesn't prevent other modules from working.  Thus,
  //     we use APR_HOOK_LAST for our position argument.
  //   * In particular, we MUST run after mod_ssl's pre-connection hook, so
  //     that we can ask mod_ssl if this connection is using SSL.  Thus, we
  //     name mod_ssl.c as a predecessor.  This is redundant, since mod_ssl's
  //     pre-connection hook uses APR_HOOK_MIDDLE, but it's good to be sure.
  // For more about controlling hook order, see TAMB 10.2.2 or
  // http://httpd.apache.org/docs/trunk/developer/hooks.html#hooking-order
  ap_hook_pre_connection(
      PreConnection,              // hook function to be called
      modules_mod_ssl,            // predecessors
      modules_core,               // successors
      APR_HOOK_LAST);             // position

  // Register our process-connection hook, which will handle SPDY connections.
  // The first process-connection hook in the chain to return OK gets to be in
  // charge of handling the connection from start to finish, so we put
  // ourselves in APR_HOOK_FIRST so we can get an early look at the connection.
  // If it turns out not to be a SPDY connection, we'll get out of the way and
  // let other modules deal with it.
  ap_hook_process_connection(
      ProcessConnection,          // hook function to be called
      NULL,                       // predecessors
      NULL,                       // successors
      APR_HOOK_FIRST);            // position

  // Register a hook to be called when adding filters for each new request.
  // This hook will insert our HTTP-to-SPDY and anti-chunking filter into our
  // slave connections.
  ap_hook_insert_filter(
      InsertRequestFilters,       // hook function to be called
      NULL,                       // predecessors
      NULL,                       // successors
      APR_HOOK_MIDDLE);           // position

  // Register a hook with mod_ssl to be called when deciding what protocols to
  // advertise during Next Protocol Negotiatiation (NPN); we'll use this
  // opportunity to advertise that we support SPDY.  This hook is declared in
  // mod_ssl.h, for appropriately-patched versions of mod_ssl.  See TAMB 10.2.3
  // for more about optional hooks.
  APR_OPTIONAL_HOOK(
      ssl,                        // prefix of optional hook
      npn_advertise_protos_hook,  // name of optional hook
      AdvertiseNpnProtocols,      // hook function to be called
      NULL,                       // predecessors
      NULL,                       // successors
      APR_HOOK_MIDDLE);           // position

  // Register a hook with mod_ssl to be called when NPN has been completed and
  // the next protocol decided upon.  This hook will check if we're actually to
  // be using SPDY with the client, and enable this module if so.  This hook is
  // declared in mod_ssl.h, for appropriately-patched versions of mod_ssl.
  APR_OPTIONAL_HOOK(
      ssl,                        // prefix of optional hook
      npn_proto_negotiated_hook,  // name of optional hook
      OnNextProtocolNegotiated,   // hook function to be called
      NULL,                       // predecessors
      NULL,                       // successors
      APR_HOOK_MIDDLE);           // position

  // Register our input filter, and store the filter handle into a global
  // variable so we can use it later to instantiate our filter into a filter
  // chain.  The "filter type" argument below determines where in the filter
  // chain our filter will be placed.  We use AP_FTYPE_NETWORK so that we will
  // be at the very end of the input chain for slave connections, in place of
  // the usual core input filter.
  gSpdyToHttpFilterHandle = ap_register_input_filter(
      "SPDY_TO_HTTP",             // name
      SpdyToHttpFilter,           // filter function
      NULL,                       // init function (n/a in our case)
      AP_FTYPE_NETWORK);          // filter type

  // Now register our output filter, analogously to the input filter above.
  // Using AP_FTYPE_TRANSCODE allows us to convert from HTTP to SPDY at the end
  // of the protocol phase, so that we still have access to the HTTP headers as
  // a data structure (rather than raw bytes).  See TAMB 8.2 for a summary of
  // the different filter types.
  //
  // Even though we use AP_FTYPE_TRANSCODE, we expect to be the last filter in
  // the chain for slave connections, because we explicitly disable mod_ssl and
  // the core output filter for slave connections.  However, if another module
  // exists that uses a connection-level output filter, it may not work with
  // mod_spdy.  We should revisit this if that becomes a problem.
  gHttpToSpdyFilterHandle = ap_register_output_filter(
      "HTTP_TO_SPDY",             // name
      HttpToSpdyFilter,           // filter function
      NULL,                       // init function (n/a in our case)
      AP_FTYPE_TRANSCODE);        // filter type

  // This output filter is a hack to ensure that Httpd doesn't try to chunk our
  // output data (which would _not_ mix well with SPDY).  Using a filter type
  // of PROTOCOL-1 ensures that it runs just before the core HTTP_HEADER filter
  // (which is responsible for inserting the CHUNK filter).
  gAntiChunkingFilterHandle = ap_register_output_filter(
      "SPDY_ANTI_CHUNKING", AntiChunkingFilter, NULL,
      static_cast<ap_filter_type>(AP_FTYPE_PROTOCOL - 1));
}

}  // namespace

extern "C" {

  // Export our module so Apache is able to load us.
  // See http://gcc.gnu.org/wiki/Visibility for more information.
#if defined(__linux)
#pragma GCC visibility push(default)
#endif

  // Declare our module object (note that "module" is a typedef for "struct
  // module_struct"; see http_config.h for the definition of module_struct).
  module AP_MODULE_DECLARE_DATA spdy_module = {
    // This next macro indicates that this is a (non-MPM) Apache 2.0 module
    // (the macro actually expands to multiple comma-separated arguments; see
    // http_config.h for the definition):
    STANDARD20_MODULE_STUFF,

    // These next four arguments are callbacks for manipulating configuration
    // structures (the ones we don't need are left null):
    NULL,  // create per-directory config structure
    NULL,  // merge per-directory config structures
    mod_spdy::CreateSpdyServerConfig,  // create per-server config structure
    mod_spdy::MergeSpdyServerConfigs,  // merge per-server config structures

    // This argument supplies a table describing the configuration directives
    // implemented by this module:
    mod_spdy::kSpdyConfigCommands,

    // Finally, this function will be called to register hooks for this module:
    RegisterHooks
  };

#if defined(__linux)
#pragma GCC visibility pop
#endif

}
