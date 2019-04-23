/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*!
 * @file OpflexListener.h
 * @brief Interface definition file for OpflexListener
 */
/*
 * Copyright (c) 2014 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#include <set>
#include <netinet/in.h>

#include <uv.h>
#include <cstdint>

#include "opflex/engine/internal/OpflexServerConnection.h"
#include "opflex/engine/internal/OpflexMessage.h"
#include "opflex/ofcore/OFTypes.h"
#include "yajr/transport/ZeroCopyOpenSSL.hpp"
#include "opflex/modb/internal/ObjectStore.h"

#pragma once
#ifndef OPFLEX_ENGINE_OPFLEXLISTENER_H
#define OPFLEX_ENGINE_OPFLEXLISTENER_H

#if HAVE_CONFIG_H
#  include <config.h>
#endif

namespace opflex {
namespace engine {
namespace internal {

class OpflexPool;

/**
 * Represents a listen socket that will spawn OpflexServerConnections
 */
class OpflexListener {
public:
    typedef std::set<uint64_t> conn_set_t;

    /**
     * Create a new opflex listener for the given IP address and port
     *
     * @param handlerFactory a factory that can allocate a handler for
     * the spawned OpflexServerConnection objects
     * @param port the TCP port number to bind to
     * @param name the opflex name for the server
     * @param domain the opflex domain for the server
     */
    OpflexListener(HandlerFactory& handlerFactory,
                   int port,
                   const std::string& name,
                   const std::string& domain);

    /**
     * Create a new opflex listener for the given UNIX domain socket
     * path.
     *
     * @param handlerFactory a factory that can allocate a handler for
     * the spawned OpflexServerConnection objects
     * @param socketName The path to the UNIX domain socket
     * @param name the opflex name for the server
     * @param domain the opflex domain for the server
     */
    OpflexListener(HandlerFactory& handlerFactory,
                   const std::string& socketName,
                   const std::string& name,
                   const std::string& domain);
    ~OpflexListener();

    /**
     * Enable SSL for connections to opflex peers.  Call before listen().
     *
     * @param caStorePath the filesystem path to a directory
     * containing CA certificates, or to a file containing a specific
     * CA certificate.
     * @param serverKeyPath the path to the server private key
     * @param serverKeyPass the passphrase for the server private key
     * @param verifyPeers set to true to verify that peer certificates
     * properly chain to a trusted root
     */
    void enableSSL(const std::string& caStorePath,
                   const std::string& serverKeyPath,
                   const std::string& serverKeyPass,
                   bool verifyPeers = true);

    /**
     * Start listening on the local socket for new connections
     */
    void listen();

    /**
     * Stop listening on the local socket for new connections
     */
    void disconnect();

    /**
     * Get the bind port for this connection
     */
    int getPort() const { return port; }

    /**
     * Get the unique name for this component in the policy domain
     *
     * @returns the string name
     */
    const std::string& getName() { return name; }

    /**
     * Get the globally unique name for this policy domain
     *
     * @returns the string domain name
     */
    const std::string& getDomain() { return domain; }

    /**
     * Send a given message to all the connected and ready peers.
     *
     * @param message the message to write.  The memory will be owned
     * by the listener
     */
    void sendToAll(OpflexMessage* message);

    /**
     * Send a given message to all the connected and ready peers
     * that are subscribed to the uri.
     *
     * @param message the message to write.  The memory will be owned
     * by the listener
     */
    void sendToListeners(const std::string& uri, OpflexMessage* message);

    /**
     * Send a given message to a set of listeners.
     *
     * @param set of clients and request to send.
     */
    void sendToListeners(const std::vector<modb::reference_t>& mo,
                         OpflexMessage* message);

    /**
     * A predicate for use with applyConnPred
     */
    typedef bool (*conn_pred_t)(OpflexServerConnection* conn, void* user);

    /**
     * Apply the given predicate to all connection objects, returning
     * true if the predicate is true for all connections
     */
    bool applyConnPred(conn_pred_t pred, void* user);

    /**
     * Check whether the server is listening on the socket
     * @return true if the server is listening
     */
    bool isListening();

    /**
     * Return unique connection id
     */
    uint64_t getUniqueConnId(void) { return conn_id++; }

    /**
     * update database of resolved URIs
     */
     void resolvedUri(const std::string& uri, uint64_t conn_id);
     void unResolvedUri(const std::string& uri, uint64_t conn_id);

    /**
     * garbage collect stale uris
     */
    void onCleanupTimer(void);

private:
    HandlerFactory& handlerFactory;

    std::string socketName;
    int port;

#ifdef HAVE_CXX11
    std::unique_ptr<yajr::transport::ZeroCopyOpenSSL::Ctx> serverCtx;
#else
    std::auto_ptr<yajr::transport::ZeroCopyOpenSSL::Ctx> serverCtx;
#endif

    std::string name;
    std::string domain;

    volatile bool active;

    uv_loop_t server_loop;
    uv_thread_t server_thread;

    yajr::Listener* listener;

    uv_mutex_t conn_mutex;
    uv_key_t conn_mutex_key;
    typedef std::map<uint64_t, OpflexServerConnection*> conn_map_t;
    conn_map_t conns;

    uint64_t conn_id;

    typedef OF_UNORDERED_MAP<std::string, conn_set_t> resolv_uri_map_t;
    resolv_uri_map_t resolv_uri_map;

    uv_async_t cleanup_async;
    uv_async_t writeq_async;

    static void server_thread_func(void* processor);
    static void on_cleanup_async(uv_async_t *handle);
    static void on_writeq_async(uv_async_t *handle);
    void messagesReady();
    uv_loop_t* getLoop() { return &server_loop; }
    void connectionClosed(OpflexServerConnection* conn);

    static void* on_new_connection(yajr::Listener* listener,
                                   void* data, int error);

    friend class OpflexServerConnection;
};


} /* namespace internal */
} /* namespace engine */
} /* namespace opflex */

#endif /* OPFLEX_ENGINE_OPFLEXCONNECTION_H */
