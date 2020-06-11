
/*
 * Copyright (c) 2014-2019 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#include "SpanRenderer.h"
#include "OvsdbState.h"
#include <opflexagent/logging.h>
#include <opflexagent/SpanManager.h>
#include <boost/optional.hpp>


namespace opflexagent {
    using boost::optional;
    using namespace std;
    using modelgbp::gbp::DirectionEnumT;

    SpanRenderer::SpanRenderer(Agent& agent_) : JsonRpcRenderer(agent_) {}

    void SpanRenderer::start(const std::string& swName, OvsdbConnection* conn) {
        LOG(DEBUG) << "starting span renderer";
        JsonRpcRenderer::start(swName, conn);
        agent.getSpanManager().registerListener(this);
    }

    void SpanRenderer::stop() {
        LOG(DEBUG) << "stopping span renderer";
        agent.getSpanManager().unregisterListener(this);
    }

    void SpanRenderer::spanUpdated(const opflex::modb::URI& spanURI) {
        LOG(INFO) << "span updated " << spanURI;
        handleSpanUpdate(spanURI);
    }

    void SpanRenderer::spanDeleted(const shared_ptr<SessionState>& seSt) {
        if (!connect()) {
            LOG(DEBUG) << "failed to connect, retry in " << CONNECTION_RETRY << " seconds";
            // connection failed, start a timer to try again
            connection_timer.reset(new deadline_timer(agent.getAgentIOService(),
                                                      boost::posix_time::seconds(CONNECTION_RETRY)));
            connection_timer->async_wait(boost::bind(&SpanRenderer::delConnectPtrCb, this,
                                                     boost::asio::placeholders::error, seSt));
            timerStarted = true;
            return;
        }
        sessionDeleted(seSt->getName());
    }

    void SpanRenderer::updateConnectCb(const boost::system::error_code& ec,
            const opflex::modb::URI& spanURI) {
        LOG(DEBUG) << "timer update cb";
        if (ec) {
            string cat = string(ec.category().name());
            LOG(DEBUG) << "timer error " << cat << ":" << ec.value();
            if (!(cat == "system" &&
                ec.value() == 125)) {
                connection_timer->cancel();
                timerStarted = false;
            }
            return;
        }
        spanUpdated(spanURI);
    }

    void SpanRenderer::delConnectPtrCb(const boost::system::error_code& ec, const shared_ptr<SessionState>& pSt) {
        if (ec) {
            connection_timer.reset();
            return;
        }
        LOG(DEBUG) << "timer span del with ptr cb";
        spanDeleted(pSt);
    }

    void SpanRenderer::sessionDeleted(const string& sessionName) {
        LOG(INFO) << "deleting session " << sessionName;
        deleteMirror(sessionName);
        LOG(INFO) << "deleting erspan port " << (ERSPAN_PORT_PREFIX + sessionName);
        deleteErspanPort(ERSPAN_PORT_PREFIX + sessionName);
    }

    void SpanRenderer::handleSpanUpdate(const opflex::modb::URI& spanURI) {
        SpanManager& spMgr = agent.getSpanManager();
        lock_guard<recursive_mutex> guard(opflexagent::SpanManager::updates);
        optional<shared_ptr<SessionState>> seSt = spMgr.getSessionState(spanURI);
        // Is the session state pointer set
        if (!seSt) {
            return;
        }

        if (!connect()) {
            LOG(DEBUG) << "failed to connect, retry in " << CONNECTION_RETRY << " seconds";
            // connection failed, start a timer to try again

            connection_timer.reset(new deadline_timer(agent.getAgentIOService(),
                                                      milliseconds(CONNECTION_RETRY * 1000)));
            connection_timer->async_wait(boost::bind(&SpanRenderer::updateConnectCb, this,
                                                     boost::asio::placeholders::error, spanURI));
            timerStarted = true;
            LOG(DEBUG) << "conn timer " << connection_timer << ", timerStarted: " << timerStarted;
            return;
        }

        // get mirror artifacts from OVSDB if provisioned
        opflexagent::mirror mir;
        bool isMirProv = conn->getOvsdbState().getMirrorState(seSt.get()->getName(), mir);
        if (isMirProv) {
            LOG(DEBUG) << "mirror state for " << seSt.get()->getName() << " uuid " << mir.uuid;
            LOG(DEBUG) << "src ports = " << mir.src_ports.size() << ", dst ports = " << mir.dst_ports.size();
        }

        // There should be at least one source and the destination should be set
        // Admin state should be ON.
        if (seSt.get()->hasSrcEndpoints() ||
            !seSt.get()->getDestination().is_unspecified() ||
            seSt.get()->getAdminState() == 0) {
            if (isMirProv) {
                sessionDeleted(seSt.get()->getName());
            }
        } else {
            LOG(INFO) << "Incomplete mirror config. Either admin down or missing src/dest EPs";
            return;
        }

        //get the source ports.
        set<string> srcPort;
        set<string> dstPort;

        SessionState::srcEpSet srcEps;
        seSt.get()->getSrcEndpointSet(srcEps);
        for (auto& src : srcEps) {
            if (src.getDirection() == DirectionEnumT::CONST_BIDIRECTIONAL ||
                    src.getDirection() == DirectionEnumT::CONST_OUT) {
                srcPort.emplace(src.getPort());
            }
            if (src.getDirection() == DirectionEnumT::CONST_BIDIRECTIONAL ||
                    src.getDirection() == DirectionEnumT::CONST_IN) {
                dstPort.emplace(src.getPort());
            }
        }

        LOG(DEBUG) << "src port count = " << srcPort.size();
        LOG(DEBUG) << "dest port count = " << dstPort.size();

        // check if the number of source and dest ports are the
        // same as provisioned.
        if (srcPort.size() != mir.src_ports.size() ||
                dstPort.size() != mir.dst_ports.size()) {
            LOG(DEBUG) << "updating mirror config";
            updateMirrorConfig(seSt.get());
            return;
        }

        // compare source port names. If at least one is different, the config
        // has changed.
        for (const auto& src_port : mir.src_ports) {
            auto itr = srcPort.find(src_port);
            if (itr != srcPort.end()) {
                srcPort.erase(itr);
            } else {
                updateMirrorConfig(seSt.get());
                return;
            }
        }
        if (!srcPort.empty()) {
            updateMirrorConfig(seSt.get());
            return;
        }

        for (const auto& dst_port : mir.dst_ports) {
            auto itr = dstPort.find(dst_port);
            if (itr != dstPort.end()) {
                dstPort.erase(itr);
            } else {
                updateMirrorConfig(seSt.get());
                return;
            }
        }
        if (!dstPort.empty()) {
            updateMirrorConfig(seSt.get());
            return;
        }

        // get ERSPAN interface params if configured
        ErspanParams params;
        if (!conn->getOvsdbState().getErspanParams(ERSPAN_PORT_PREFIX + seSt.get()->getName(), params)) {
            LOG(DEBUG) << "Unable to get ERSPAN parameters";
            return;
        }

        // check for change in config, push it if there is a change.
        if (params.getRemoteIp() != seSt.get()->getDestination().to_string() ||
            params.getVersion() != seSt.get()->getVersion()) {
            LOG(INFO) << "Mirror config has changed for " << seSt.get()->getName();
            updateMirrorConfig(seSt.get());
            return;
        }
    }

    void SpanRenderer::updateMirrorConfig(const shared_ptr<SessionState>& seSt) {
        // get the source ports.
        set<string> srcPort;
        set<string> dstPort;
        SessionState::srcEpSet srcEps;
        seSt->getSrcEndpointSet(srcEps);
        for (auto& src : srcEps) {
            if (src.getDirection() == DirectionEnumT::CONST_BIDIRECTIONAL ||
                src.getDirection() == DirectionEnumT::CONST_OUT) {
                srcPort.emplace(src.getPort());
            }
            if (src.getDirection() == DirectionEnumT::CONST_BIDIRECTIONAL ||
                src.getDirection() == DirectionEnumT::CONST_IN) {
                dstPort.emplace(src.getPort());
            }
        }
        LOG(DEBUG) << "Updating mirror config with srcport count = " << srcPort.size() << " and dstport count = " << dstPort.size();

        deleteErspanPort(ERSPAN_PORT_PREFIX + seSt->getName());
        addErspanPort(ERSPAN_PORT_PREFIX + seSt->getName(), seSt->getDestination().to_string(), seSt->getVersion());
        LOG(DEBUG) << "creating mirror";
        deleteMirror(seSt->getName());
        createMirror(seSt->getName(), srcPort, dstPort);
    }

    void SpanRenderer::deleteMirror(const string& sessionName) {
        LOG(DEBUG) << "deleting mirror " << sessionName;
        string sessionUuid;
        conn->getOvsdbState().getUuidForName(OvsdbTable::MIRROR, sessionName, sessionUuid);
        if (sessionUuid.empty()) {
            LOG(INFO) << "Unable to find session " << sessionName << " to delete";
            return;
        }
        OvsdbTransactMessage msg(OvsdbOperation::MUTATE, OvsdbTable::BRIDGE);
        set<tuple<string, OvsdbFunction, string>> condSet;
        condSet.emplace("name", OvsdbFunction::EQ, switchName);
        msg.conditions = condSet;

        vector<OvsdbValue> values;
        values.emplace_back("uuid", sessionUuid);
        OvsdbValues tdSet = OvsdbValues(values);
        msg.mutateRowData.emplace("mirrors", std::make_pair(OvsdbOperation::DELETE, tdSet));

        list<OvsdbTransactMessage> requests = {msg};
        sendAsyncTransactRequests(requests);
    }

    void SpanRenderer::addErspanPort(const string& portName, const string& remoteIp, const uint8_t version) {
        LOG(DEBUG) << "adding erspan port " << portName << " IP " << remoteIp << " and version " << std::to_string(version);

        OvsdbTransactMessage msg1(OvsdbOperation::INSERT, OvsdbTable::PORT);
        vector<OvsdbValue> values;
        values.emplace_back(portName);
        OvsdbValues tdSet(values);
        msg1.rowData.emplace("name", tdSet);

        // uuid-name
        const string uuid_name = "port1";
        msg1.externalKey = make_pair("uuid-name", uuid_name);

        // interfaces
        values.clear();
        const string named_uuid = "interface1";
        values.emplace_back("named-uuid", named_uuid);
        OvsdbValues tdSet2(values);
        msg1.rowData.emplace("interfaces", tdSet2);

        // uuid-name
        OvsdbTransactMessage msg2(OvsdbOperation::INSERT, OvsdbTable::INTERFACE);
        msg2.externalKey = make_pair("uuid-name", named_uuid);

        // row entries
        // name
        values.clear();
        values.emplace_back(portName);
        OvsdbValues tdSet3(values);
        msg2.rowData.emplace("name", tdSet3);

        values.clear();
        const string typeString("erspan");
        OvsdbValue typeData(typeString);
        values.push_back(typeData);
        OvsdbValues tdSet4(values);
        msg2.rowData.emplace("type", tdSet4);

        values.clear();
        values.emplace_back("erspan_ver", std::to_string(version));
        values.emplace_back("remote_ip", remoteIp);
        OvsdbValues tdSet5("map", values);
        msg2.rowData.emplace("options", tdSet5);

        OvsdbTransactMessage msg3(OvsdbOperation::MUTATE, OvsdbTable::BRIDGE);
        values.clear();
        values.emplace_back("named-uuid", uuid_name);
        OvsdbValues tdSet6(values);
        msg3.mutateRowData.emplace("ports", std::make_pair(OvsdbOperation::INSERT, tdSet6));
        set<tuple<string, OvsdbFunction, string>> condSet;
        condSet.emplace("name", OvsdbFunction::EQ, switchName);
        msg3.conditions = condSet;

        const list<OvsdbTransactMessage> requests = {msg1, msg2, msg3};
        sendAsyncTransactRequests(requests);
    }

    void SpanRenderer::deleteErspanPort(const string& name) {
        LOG(DEBUG) << "deleting erspan port " << name;
        string erspanUuid;
        conn->getOvsdbState().getUuidForName(OvsdbTable::PORT, name, erspanUuid);
        if (erspanUuid.empty()) {
            LOG(DEBUG) << "Port is not present in OVSDB: " << name;
            return;
        }
        LOG(DEBUG) << name << " port uuid: " << erspanUuid;
        OvsdbTransactMessage msg1(OvsdbOperation::MUTATE, OvsdbTable::BRIDGE);
        set<tuple<string, OvsdbFunction, string>> condSet;
        condSet.emplace("name", OvsdbFunction::EQ, switchName);
        msg1.conditions = condSet;

        vector<OvsdbValue> values;
        values.emplace_back("uuid", erspanUuid);
        OvsdbValues tdSet = OvsdbValues(values);
        msg1.mutateRowData.emplace("ports", std::make_pair(OvsdbOperation::DELETE, tdSet));
        LOG(DEBUG) << "deleting " << erspanUuid;
        const list<OvsdbTransactMessage> requests = {msg1};
        sendAsyncTransactRequests(requests);
    }

    void SpanRenderer::createMirror(const string& sess, const set<string>& srcPorts, const set<string>& dstPorts) {
        string brUuid;
        conn->getOvsdbState().getBridgeUuid(switchName, brUuid);
        LOG(DEBUG) << "bridge uuid " << brUuid;

        OvsdbTransactMessage msg1(OvsdbOperation::INSERT, OvsdbTable::MIRROR);
        vector<OvsdbValue> srcPortUuids;
        for (auto &srcPort : srcPorts) {
            string srcPortUuid;
            LOG(DEBUG) << "Looking up port " << srcPort;
            conn->getOvsdbState().getUuidForName(OvsdbTable::PORT, srcPort, srcPortUuid);
            if (!srcPortUuid.empty()) {
                LOG(DEBUG) << "uuid for port " << srcPort << " is " << srcPortUuid;
                srcPortUuids.emplace_back("uuid", srcPortUuid);
            } else {
                LOG(DEBUG) << "Unable to find uuid for port " << srcPort;
            }
        }

        LOG(INFO) << "mirror src_port size " << srcPortUuids.size();
        OvsdbValues tdSet("set", srcPortUuids);
        msg1.rowData.emplace("select_src_port", tdSet);

        // dst ports
        vector<OvsdbValue> dstPortUuids;
        for (auto &dstPort : dstPorts) {
            string dstPortUuid;
            conn->getOvsdbState().getUuidForName(OvsdbTable::PORT, dstPort, dstPortUuid);
            if (!dstPortUuid.empty()) {
                dstPortUuids.emplace_back("uuid", dstPortUuid);
            } else {
                LOG(WARNING) << "Unable to find uuid for port " << dstPort;
            }
        }
        LOG(INFO) << "mirror dst_port size " << dstPortUuids.size();
        OvsdbValues tdSet2("set", dstPortUuids);
        msg1.rowData.emplace("select_dst_port", tdSet2);

        // output ports
        string outputPortUuid;
        conn->getOvsdbState().getUuidForName(OvsdbTable::PORT, ERSPAN_PORT_PREFIX + sess, outputPortUuid);
        LOG(INFO) << "output port uuid " << outputPortUuid;

        if (!outputPortUuid.empty()) {
            vector<OvsdbValue> outputPort;
            OvsdbValue outPort("uuid", outputPortUuid);
            outputPort.emplace_back(outPort);
            OvsdbValues tdSet3(outputPort);
            msg1.rowData.emplace("output_port", tdSet3);
        }

        // name
        vector<OvsdbValue> values;
        values.emplace_back(sess);
        OvsdbValues tdSet4(values);
        msg1.rowData.emplace("name", tdSet4);

        const string uuid_name = "mirror1";
        msg1.externalKey = make_pair("uuid-name", uuid_name);

        // msg2
        values.clear();
        OvsdbTransactMessage msg2(OvsdbOperation::MUTATE, OvsdbTable::BRIDGE);
        set<tuple<string, OvsdbFunction, string>> condSet;
        condSet.emplace("_uuid", OvsdbFunction::EQ, brUuid);
        msg2.conditions = condSet;
        values.emplace_back("named-uuid", uuid_name);
        OvsdbValues tdSet5(values);
        msg2.mutateRowData.emplace("mirrors", std::make_pair(OvsdbOperation::INSERT, tdSet5));

        const list<OvsdbTransactMessage> requests = {msg1, msg2};
        sendAsyncTransactRequests(requests);
    }
}
