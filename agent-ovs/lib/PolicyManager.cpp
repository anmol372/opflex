/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*
 * Implementation for PolicyManager class.
 *
 * Copyright (c) 2014 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#include <algorithm>

#include <modelgbp/gbp/UnknownFloodModeEnumT.hpp>
#include <modelgbp/gbp/RoutingModeEnumT.hpp>
#include <modelgbp/gbp/DirectionEnumT.hpp>
#include <modelgbp/gbp/HashingAlgorithmEnumT.hpp>
#include <opflex/modb/URIBuilder.h>

#include <opflexagent/logging.h>
#include <opflexagent/PolicyManager.h>
#include "FlowUtils.h"

namespace opflexagent {

using std::vector;
using std::string;
using std::shared_ptr;
using std::make_shared;
using std::unordered_set;
using std::unique_lock;
using std::lock_guard;
using std::mutex;
using opflex::modb::Mutator;
using opflex::ofcore::OFFramework;
using opflex::modb::class_id_t;
using opflex::modb::URI;
using opflex::modb::URIBuilder;
using boost::optional;
using boost::asio::ip::address;

PolicyManager::PolicyManager(OFFramework& framework_,
                             boost::asio::io_service& agent_io_)
    : framework(framework_), opflexDomain("default"), taskQueue(agent_io_),
      domainListener(*this), contractListener(*this),
      secGroupListener(*this), configListener(*this) {

}

PolicyManager::~PolicyManager() {

}

const uint16_t PolicyManager::MAX_POLICY_RULE_PRIORITY = 8192;

void PolicyManager::start() {
    LOG(DEBUG) << "Starting policy manager";

    using namespace modelgbp;
    using namespace modelgbp::gbp;
    using namespace modelgbp::gbpe;

    platform::Config::registerListener(framework, &configListener);

    BridgeDomain::registerListener(framework, &domainListener);
    FloodDomain::registerListener(framework, &domainListener);
    FloodContext::registerListener(framework, &domainListener);
    RoutingDomain::registerListener(framework, &domainListener);
    Subnets::registerListener(framework, &domainListener);
    Subnet::registerListener(framework, &domainListener);
    EpGroup::registerListener(framework, &domainListener);
    L3ExternalNetwork::registerListener(framework, &domainListener);

    EpGroup::registerListener(framework, &contractListener);
    L3ExternalNetwork::registerListener(framework, &contractListener);
    RoutingDomain::registerListener(framework, &contractListener);
    Contract::registerListener(framework, &contractListener);
    Subject::registerListener(framework, &contractListener);
    Rule::registerListener(framework, &contractListener);
    L24Classifier::registerListener(framework, &contractListener);
    RedirectDestGroup::registerListener(framework, &contractListener);
    RedirectDest::registerListener(framework, &contractListener);
    RedirectAction::registerListener(framework, &contractListener);

    SecGroup::registerListener(framework, &secGroupListener);
    SecGroupSubject::registerListener(framework, &secGroupListener);
    SecGroupRule::registerListener(framework, &secGroupListener);
    L24Classifier::registerListener(framework, &secGroupListener);
    Subnets::registerListener(framework, &secGroupListener);
    Subnet::registerListener(framework, &secGroupListener);

    // resolve platform config
    Mutator mutator(framework, "init");
    optional<shared_ptr<dmtree::Root> >
        root(dmtree::Root::resolve(framework, URI::ROOT));
    if (root)
        root.get()->addDomainConfig()
            ->addDomainConfigToConfigRSrc()
            ->setTargetConfig(opflexDomain);
    mutator.commit();
}

void PolicyManager::stop() {
    LOG(DEBUG) << "Stopping policy manager";

    using namespace modelgbp;
    using namespace modelgbp::gbp;
    using namespace modelgbp::gbpe;
    BridgeDomain::unregisterListener(framework, &domainListener);
    FloodDomain::unregisterListener(framework, &domainListener);
    FloodContext::unregisterListener(framework, &domainListener);
    RoutingDomain::unregisterListener(framework, &domainListener);
    Subnets::unregisterListener(framework, &domainListener);
    Subnet::unregisterListener(framework, &domainListener);
    EpGroup::unregisterListener(framework, &domainListener);
    L3ExternalNetwork::unregisterListener(framework, &domainListener);

    EpGroup::unregisterListener(framework, &contractListener);
    L3ExternalNetwork::unregisterListener(framework, &contractListener);
    RoutingDomain::unregisterListener(framework, &contractListener);
    Contract::unregisterListener(framework, &contractListener);
    Subject::unregisterListener(framework, &contractListener);
    Rule::unregisterListener(framework, &contractListener);
    L24Classifier::unregisterListener(framework, &contractListener);
    RedirectDestGroup::unregisterListener(framework, &contractListener);
    RedirectDest::unregisterListener(framework, &contractListener);
    RedirectAction::unregisterListener(framework, &contractListener);

    SecGroup::unregisterListener(framework, &secGroupListener);
    SecGroupSubject::unregisterListener(framework, &secGroupListener);
    SecGroupRule::unregisterListener(framework, &secGroupListener);
    L24Classifier::unregisterListener(framework, &secGroupListener);
    Subnets::unregisterListener(framework, &secGroupListener);
    Subnet::unregisterListener(framework, &secGroupListener);

    lock_guard<mutex> guard(state_mutex);
    group_map.clear();
    vnid_map.clear();
    redirGrpMap.clear();
}

void PolicyManager::registerListener(PolicyListener* listener) {
    lock_guard<mutex> guard(listener_mutex);
    policyListeners.push_back(listener);
}

void PolicyManager::unregisterListener(PolicyListener* listener) {
    lock_guard<mutex> guard(listener_mutex);
    policyListeners.remove(listener);
}

void PolicyManager::notifyEPGDomain(const URI& egURI) {
    lock_guard<mutex> guard(listener_mutex);
    for (PolicyListener* listener : policyListeners) {
        listener->egDomainUpdated(egURI);
    }
}

void PolicyManager::notifyDomain(class_id_t cid, const URI& domURI) {
    lock_guard<mutex> guard(listener_mutex);
    for (PolicyListener* listener : policyListeners) {
        listener->domainUpdated(cid, domURI);
    }
}

void PolicyManager::notifyContract(const URI& contractURI) {
    lock_guard<mutex> guard(listener_mutex);
    for (PolicyListener *listener : policyListeners) {
        listener->contractUpdated(contractURI);
    }
}

void PolicyManager::notifySecGroup(const URI& secGroupURI) {
    lock_guard<mutex> guard(listener_mutex);
    for (PolicyListener *listener : policyListeners) {
        listener->secGroupUpdated(secGroupURI);
    }
}

void PolicyManager::notifyConfig(const URI& configURI) {
    lock_guard<mutex> guard(listener_mutex);
    for (PolicyListener *listener : policyListeners) {
        listener->configUpdated(configURI);
    }
}

optional<shared_ptr<modelgbp::gbp::RoutingDomain> >
PolicyManager::getRDForGroup(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    if (it == group_map.end()) return boost::none;
    return it->second.routingDomain;
}

optional<shared_ptr<modelgbp::gbp::RoutingDomain> >
PolicyManager::getRDForL3ExtNet(const opflex::modb::URI& l3n) {
    lock_guard<mutex> guard(state_mutex);
    l3n_map_t::iterator it = l3n_map.find(l3n);
    if (it == l3n_map.end()) return boost::none;
    return it->second.routingDomain;
}

optional<shared_ptr<modelgbp::gbp::BridgeDomain> >
PolicyManager::getBDForGroup(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    if (it == group_map.end()) return boost::none;
    return it->second.bridgeDomain;
}

optional<shared_ptr<modelgbp::gbp::FloodDomain> >
PolicyManager::getFDForGroup(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    if (it == group_map.end()) return boost::none;
    return it->second.floodDomain;
}

optional<shared_ptr<modelgbp::gbpe::FloodContext> >
PolicyManager::getFloodContextForGroup(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    if (it == group_map.end()) return boost::none;
    return it->second.floodContext;
}

void PolicyManager::getSubnetsForGroup(const opflex::modb::URI& eg,
                                       /* out */ subnet_vector_t& subnets) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    if (it == group_map.end()) return;
    for (const GroupState::subnet_map_t::value_type& v :
             it->second.subnet_map) {
        subnets.push_back(v.second);
    }
}

optional<shared_ptr<modelgbp::gbp::Subnet> >
PolicyManager::findSubnetForEp(const opflex::modb::URI& eg,
                               const address& ip) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    if (it == group_map.end()) return boost::none;
    boost::system::error_code ec;
    for (const GroupState::subnet_map_t::value_type& v :
             it->second.subnet_map) {
        if (!v.second->isAddressSet() || !v.second->isPrefixLenSet())
            continue;
        address netAddr =
            address::from_string(v.second->getAddress().get(), ec);
        uint8_t prefixLen = v.second->getPrefixLen().get();

        if (netAddr.is_v4() != ip.is_v4()) continue;

        if (netAddr.is_v4()) {
            if (prefixLen > 32) prefixLen = 32;

            uint32_t mask = (prefixLen != 0)
                ? (~((uint32_t)0) << (32 - prefixLen))
                : 0;
            uint32_t net_addr = netAddr.to_v4().to_ulong() & mask;
            uint32_t ip_addr = ip.to_v4().to_ulong() & mask;

            if (net_addr == ip_addr)
                return v.second;
        } else {
            if (prefixLen > 128) prefixLen = 128;

            struct in6_addr mask;
            struct in6_addr net_addr;
            struct in6_addr ip_addr;
            memcpy(&ip_addr, ip.to_v6().to_bytes().data(), sizeof(ip_addr));
            network::compute_ipv6_subnet(netAddr.to_v6(), prefixLen,
                                         &mask, &net_addr);

            ((uint64_t*)&ip_addr)[0] &= ((uint64_t*)&mask)[0];
            ((uint64_t*)&ip_addr)[1] &= ((uint64_t*)&mask)[1];

            if (((uint64_t*)&ip_addr)[0] == ((uint64_t*)&net_addr)[0] &&
                ((uint64_t*)&ip_addr)[1] == ((uint64_t*)&net_addr)[1])
                return v.second;
        }
    }
    return boost::none;
}

bool PolicyManager::updateEPGDomains(const URI& egURI, bool& toRemove) {
    using namespace modelgbp;
    using namespace modelgbp::gbp;
    using namespace modelgbp::gbpe;

    GroupState& gs = group_map[egURI];

    optional<shared_ptr<EpGroup> > epg =
        EpGroup::resolve(framework, egURI);
    if (!epg) {
        toRemove = true;
        return true;
    }
    toRemove = false;

    optional<shared_ptr<InstContext> > newInstCtx =
        epg.get()->resolveGbpeInstContext();
    if (gs.instContext && gs.instContext.get()->getEncapId()) {
        vnid_map.erase(gs.instContext.get()->getEncapId().get());
    }
    if (newInstCtx && newInstCtx.get()->getEncapId()) {
        vnid_map.insert(std::make_pair(newInstCtx.get()->getEncapId().get(),
                                       egURI));
    }

    optional<shared_ptr<RoutingDomain> > newrd;
    optional<shared_ptr<BridgeDomain> > newbd;
    optional<shared_ptr<FloodDomain> > newfd;
    optional<shared_ptr<FloodContext> > newfdctx;
    GroupState::subnet_map_t newsmap;
    optional<shared_ptr<EndpointRetention> > newl2epretpolicy;
    optional<shared_ptr<EndpointRetention> > newl3epretpolicy;
    optional<URI> nEpRetURI;

    optional<class_id_t> domainClass;
    optional<URI> domainURI;
    optional<shared_ptr<EpGroupToNetworkRSrc> > ref =
        epg.get()->resolveGbpEpGroupToNetworkRSrc();
    if (ref) {
        domainClass = ref.get()->getTargetClass();
        domainURI = ref.get()->getTargetURI();
    }

    // Update the subnet map for the group with the subnets directly
    // referenced by the group.
    optional<shared_ptr<EpGroupToSubnetsRSrc> > egSns =
        epg.get()->resolveGbpEpGroupToSubnetsRSrc();
    if (egSns && egSns.get()->isTargetSet()) {
        optional<shared_ptr<Subnets> > sns =
            Subnets::resolve(framework,
                             egSns.get()->getTargetURI().get());
        if (sns) {
            vector<shared_ptr<Subnet> > csns;
            sns.get()->resolveGbpSubnet(csns);
            for (shared_ptr<Subnet>& csn : csns)
                newsmap[csn->getURI()] = csn;
        }
    }

    optional<shared_ptr<InstContext> > newBDInstCtx =
        epg.get()->resolveGbpeInstContext();
    optional<shared_ptr<InstContext> > newRDInstCtx =
        epg.get()->resolveGbpeInstContext();

    // walk up the chain of forwarding domains
    while (domainURI && domainClass) {
        URI du = domainURI.get();
        optional<class_id_t> ndomainClass;
        optional<URI> ndomainURI;

        optional<shared_ptr<ForwardingBehavioralGroupToSubnetsRSrc> > fwdSns;
        switch (domainClass.get()) {
        case RoutingDomain::CLASS_ID:
            {
                newrd = RoutingDomain::resolve(framework, du);
                ndomainClass = boost::none;
                ndomainURI = boost::none;
                if (newrd) {
                    fwdSns = newrd.get()->
                        resolveGbpForwardingBehavioralGroupToSubnetsRSrc();
                    newRDInstCtx = newrd.get()->resolveGbpeInstContext();
                    if(newRDInstCtx) {
                        optional<shared_ptr<InstContextToEpRetentionRSrc> > dref2 =
                            newRDInstCtx.get()->resolveGbpeInstContextToEpRetentionRSrc();
                        if(dref2) {
                            nEpRetURI = dref2.get()->getTargetURI();
                            newl3epretpolicy =
                                EndpointRetention::resolve(framework, nEpRetURI.get());
                        }
                    }
                }
            }
            break;
        case BridgeDomain::CLASS_ID:
            {
                newbd = BridgeDomain::resolve(framework, du);
                if (newbd) {
                    optional<shared_ptr<BridgeDomainToNetworkRSrc> > dref =
                        newbd.get()->resolveGbpBridgeDomainToNetworkRSrc();
                    if (dref) {
                        ndomainClass = dref.get()->getTargetClass();
                        ndomainURI = dref.get()->getTargetURI();
                    }
                    fwdSns = newbd.get()->
                        resolveGbpForwardingBehavioralGroupToSubnetsRSrc();
                    newBDInstCtx = newbd.get()->resolveGbpeInstContext();
                    if(newBDInstCtx) {
                        optional<shared_ptr<InstContextToEpRetentionRSrc> > dref2 =
                            newBDInstCtx.get()->resolveGbpeInstContextToEpRetentionRSrc();
                        if(dref2) {
                            nEpRetURI = dref2.get()->getTargetURI();
                            newl2epretpolicy =
                                EndpointRetention::resolve(framework, nEpRetURI.get());
                        }
                    }
                }
            }
            break;
        case FloodDomain::CLASS_ID:
            {
                newfd = FloodDomain::resolve(framework, du);
                if (newfd) {
                    optional<shared_ptr<FloodDomainToNetworkRSrc> > dref =
                        newfd.get()->resolveGbpFloodDomainToNetworkRSrc();
                    if (dref) {
                        ndomainClass = dref.get()->getTargetClass();
                        ndomainURI = dref.get()->getTargetURI();
                    }
                    newfdctx = newfd.get()->resolveGbpeFloodContext();
                    fwdSns = newfd.get()->
                        resolveGbpForwardingBehavioralGroupToSubnetsRSrc();
                }
            }
            break;
        }

        // Update the subnet map for the group with all the subnets it
        // could access.
        if (fwdSns && fwdSns.get()->isTargetSet()) {
            optional<shared_ptr<Subnets> > sns =
                Subnets::resolve(framework,
                                 fwdSns.get()->getTargetURI().get());
            if (sns) {
                vector<shared_ptr<Subnet> > csns;
                sns.get()->resolveGbpSubnet(csns);
                for (shared_ptr<Subnet>& csn : csns)
                    newsmap[csn->getURI()] = csn;
            }
        }

        domainClass = ndomainClass;
        domainURI = ndomainURI;
    }

    bool updated = false;
    if (epg != gs.epGroup ||
        newInstCtx != gs.instContext ||
        newfd != gs.floodDomain ||
        newfdctx != gs.floodContext ||
        newbd != gs.bridgeDomain ||
        newrd != gs.routingDomain ||
        newsmap != gs.subnet_map ||
        newBDInstCtx != gs.instBDContext ||
        newRDInstCtx != gs.instRDContext ||
        newl2epretpolicy != gs.l2EpRetPolicy ||
        newl3epretpolicy != gs.l3EpRetPolicy)
        updated = true;

    gs.epGroup = epg;
    gs.instContext = newInstCtx;
    gs.floodDomain = newfd;
    gs.floodContext = newfdctx;
    gs.bridgeDomain = newbd;
    gs.routingDomain = newrd;
    gs.subnet_map = newsmap;
    gs.instBDContext = newBDInstCtx;
    gs.instRDContext = newRDInstCtx;
    gs.l2EpRetPolicy = newl2epretpolicy;
    gs.l3EpRetPolicy = newl3epretpolicy;

    return updated;
}

boost::optional<uint32_t>
PolicyManager::getVnidForGroup(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.instContext &&
        it->second.instContext.get()->getEncapId()
        ? it->second.instContext.get()->getEncapId().get()
        : optional<uint32_t>();
}

boost::optional<uint32_t>
PolicyManager::getBDVnidForGroup(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.instBDContext &&
        it->second.instContext.get()->getEncapId()
        ? it->second.instContext.get()->getEncapId().get()
        : optional<uint32_t>();
}

boost::optional<uint32_t>
PolicyManager::getRDVnidForGroup(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.instRDContext &&
        it->second.instRDContext.get()->getEncapId()
        ? it->second.instRDContext.get()->getEncapId().get()
        : optional<uint32_t>();
}

boost::optional<opflex::modb::URI>
PolicyManager::getGroupForVnid(uint32_t vnid) {
    lock_guard<mutex> guard(state_mutex);
    vnid_map_t::iterator it = vnid_map.find(vnid);
    return it != vnid_map.end() ? optional<URI>(it->second) : boost::none;
}

optional<string> PolicyManager::getMulticastIPForGroup(const URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.instContext &&
        it->second.instContext.get()->getMulticastGroupIP()
        ? it->second.instContext.get()->getMulticastGroupIP().get()
        : optional<string>();
}

optional<string> PolicyManager::getBDMulticastIPForGroup(const URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.instBDContext &&
        it->second.instBDContext.get()->getMulticastGroupIP()
        ? it->second.instBDContext.get()->getMulticastGroupIP().get()
        : optional<string>();
}

optional<string> PolicyManager::getRDMulticastIPForGroup(const URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.instRDContext &&
        it->second.instRDContext.get()->getMulticastGroupIP()
        ? it->second.instRDContext.get()->getMulticastGroupIP().get()
        : optional<string>();
}

optional<uint32_t> PolicyManager::getSclassForGroup(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.instContext
        ? it->second.instContext.get()->getClassId()
        : optional<uint32_t>();
}

optional<shared_ptr<modelgbp::gbpe::EndpointRetention>> PolicyManager::getL2EPRetentionPolicyForGroup(
const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.l2EpRetPolicy.get()
        ? it->second.l2EpRetPolicy
        : optional<std::shared_ptr<modelgbp::gbpe::EndpointRetention>>();
}

optional<shared_ptr<modelgbp::gbpe::EndpointRetention>> PolicyManager::getL3EPRetentionPolicyForGroup(
const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    group_map_t::iterator it = group_map.find(eg);
    return it != group_map.end() && it->second.l3EpRetPolicy.get()
        ? it->second.l3EpRetPolicy
        : optional<std::shared_ptr<modelgbp::gbpe::EndpointRetention>>();
}

bool PolicyManager::groupExists(const opflex::modb::URI& eg) {
    lock_guard<mutex> guard(state_mutex);
    return group_map.find(eg) != group_map.end();
}

void PolicyManager::getGroups(uri_set_t& epURIs) {
    lock_guard<mutex> guard(state_mutex);
    for (const group_map_t::value_type& kv : group_map) {
        epURIs.insert(kv.first);
    }
}

void PolicyManager::getRoutingDomains(uri_set_t& rdURIs) {
    lock_guard<mutex> guard(state_mutex);
    for (const rd_map_t::value_type& kv : rd_map) {
        rdURIs.insert(kv.first);
    }
}

bool PolicyManager::removeContractIfRequired(const URI& contractURI) {
    using namespace modelgbp::gbp;
    contract_map_t::iterator itr = contractMap.find(contractURI);
    optional<shared_ptr<Contract> > contract =
        Contract::resolve(framework, contractURI);
    if (!contract && itr != contractMap.end() &&
        itr->second.providerGroups.empty() &&
        itr->second.consumerGroups.empty() &&
        itr->second.intraGroups.empty()) {
        LOG(DEBUG) << "Removing index for contract " << contractURI;
        contractMap.erase(itr);
        return true;
    }
    return false;
}

void PolicyManager::updateGroupContracts(class_id_t groupType,
                                         const URI& groupURI,
                                         uri_set_t& updatedContracts) {
    using namespace modelgbp::gbp;
    GroupContractState& gcs = groupContractMap[groupURI];

    uri_set_t provAdded, provRemoved;
    uri_set_t consAdded, consRemoved;
    uri_set_t intraAdded, intraRemoved;

    uri_sorted_set_t newProvided;
    uri_sorted_set_t newConsumed;
    uri_sorted_set_t newIntra;

    bool remove = true;
    if (groupType == EpGroup::CLASS_ID) {
        optional<shared_ptr<EpGroup> > epg =
            EpGroup::resolve(framework, groupURI);
        if (epg) {
            remove = false;
            vector<shared_ptr<EpGroupToProvContractRSrc> > provRel;
            epg.get()->resolveGbpEpGroupToProvContractRSrc(provRel);
            vector<shared_ptr<EpGroupToConsContractRSrc> > consRel;
            epg.get()->resolveGbpEpGroupToConsContractRSrc(consRel);
            vector<shared_ptr<EpGroupToIntraContractRSrc> > intraRel;
            epg.get()->resolveGbpEpGroupToIntraContractRSrc(intraRel);

            for (shared_ptr<EpGroupToProvContractRSrc>& rel : provRel) {
                if (rel->isTargetSet()) {
                    newProvided.insert(rel->getTargetURI().get());
                }
            }
            for (shared_ptr<EpGroupToConsContractRSrc>& rel : consRel) {
                if (rel->isTargetSet()) {
                    newConsumed.insert(rel->getTargetURI().get());
                }
            }
            for (shared_ptr<EpGroupToIntraContractRSrc>& rel : intraRel) {
                if (rel->isTargetSet()) {
                    newIntra.insert(rel->getTargetURI().get());
                }
            }
        }
    } else if (groupType == L3ExternalNetwork::CLASS_ID) {
        optional<shared_ptr<L3ExternalNetwork> > l3n =
            L3ExternalNetwork::resolve(framework, groupURI);
        if (l3n) {
            remove = false;
            vector<shared_ptr<L3ExternalNetworkToProvContractRSrc> > provRel;
            l3n.get()->resolveGbpL3ExternalNetworkToProvContractRSrc(provRel);
            vector<shared_ptr<L3ExternalNetworkToConsContractRSrc> > consRel;
            l3n.get()->resolveGbpL3ExternalNetworkToConsContractRSrc(consRel);

            for (shared_ptr<L3ExternalNetworkToProvContractRSrc>& rel :
                     provRel) {
                if (rel->isTargetSet()) {
                    newProvided.insert(rel->getTargetURI().get());
                }
            }
            for (shared_ptr<L3ExternalNetworkToConsContractRSrc>& rel :
                     consRel) {
                if (rel->isTargetSet()) {
                    newConsumed.insert(rel->getTargetURI().get());
                }
            }
        }
    }
    if (remove) {
        provRemoved.insert(gcs.contractsProvided.begin(),
                           gcs.contractsProvided.end());
        consRemoved.insert(gcs.contractsConsumed.begin(),
                           gcs.contractsConsumed.end());
        intraRemoved.insert(gcs.contractsIntra.begin(),
                            gcs.contractsIntra.end());
        groupContractMap.erase(groupURI);
    } else {

#define CALC_DIFF(olds, news, added, removed)                                  \
        std::set_difference(olds.begin(), olds.end(),                          \
                news.begin(), news.end(), inserter(removed, removed.begin())); \
        std::set_difference(news.begin(), news.end(),                          \
                olds.begin(), olds.end(), inserter(added, added.begin()));

        CALC_DIFF(gcs.contractsProvided, newProvided, provAdded, provRemoved);
        CALC_DIFF(gcs.contractsConsumed, newConsumed, consAdded, consRemoved);
        CALC_DIFF(gcs.contractsIntra, newIntra, intraAdded, intraRemoved);
#undef CALC_DIFF
        gcs.contractsProvided.swap(newProvided);
        gcs.contractsConsumed.swap(newConsumed);
        gcs.contractsIntra.swap(newIntra);
    }

#define INSERT_ALL(dst, src) dst.insert(src.begin(), src.end());
    INSERT_ALL(updatedContracts, provAdded);
    INSERT_ALL(updatedContracts, provRemoved);
    INSERT_ALL(updatedContracts, consAdded);
    INSERT_ALL(updatedContracts, consRemoved);
    INSERT_ALL(updatedContracts, intraAdded);
    INSERT_ALL(updatedContracts, intraRemoved);
#undef INSERT_ALL

    for (const URI& u : provAdded) {
        contractMap[u].providerGroups.insert(groupURI);
        LOG(DEBUG) << u << ": prov add: " << groupURI;
    }
    for (const URI& u : consAdded) {
        contractMap[u].consumerGroups.insert(groupURI);
        LOG(DEBUG) << u << ": cons add: " << groupURI;
    }
    for (const URI& u : intraAdded) {
        contractMap[u].intraGroups.insert(groupURI);
        LOG(DEBUG) << u << ": intra add: " << groupURI;
    }
    for (const URI& u : provRemoved) {
        contractMap[u].providerGroups.erase(groupURI);
        LOG(DEBUG) << u << ": prov remove: " << groupURI;
        removeContractIfRequired(u);
    }
    for (const URI& u : consRemoved) {
        contractMap[u].consumerGroups.erase(groupURI);
        LOG(DEBUG) << u << ": cons remove: " << groupURI;
        removeContractIfRequired(u);
    }
    for (const URI& u : intraRemoved) {
        contractMap[u].intraGroups.erase(groupURI);
        LOG(DEBUG) << u << ": intra remove: " << groupURI;
        removeContractIfRequired(u);
    }
}

bool operator==(const PolicyRule& lhs, const PolicyRule& rhs) {
    return ((lhs.getDirection() == rhs.getDirection()) &&
            (lhs.getAllow() == rhs.getAllow()) &&
            (lhs.getRemoteSubnets() == rhs.getRemoteSubnets()) &&
            (*lhs.getL24Classifier() == *rhs.getL24Classifier()) &&
            (lhs.getRedirectDestGrpURI() == rhs.getRedirectDestGrpURI()));
}

bool operator!=(const PolicyRule& lhs, const PolicyRule& rhs) {
    return !operator==(lhs,rhs);
}

std::ostream & operator<<(std::ostream &os, const PolicyRule& rule) {
    using modelgbp::gbp::DirectionEnumT;
    using network::operator<<;

    os << "PolicyRule[classifier="
       << rule.getL24Classifier()->getURI()
       << ",allow=" << rule.getAllow()
       << ",redirect=" << rule.getRedirect()
       << ",prio=" << rule.getPriority()
       << ",direction=";

    switch (rule.getDirection()) {
    case DirectionEnumT::CONST_BIDIRECTIONAL:
        os << "bi";
        break;
    case DirectionEnumT::CONST_IN:
        os << "in";
        break;
    case DirectionEnumT::CONST_OUT:
        os << "out";
        break;
    }

    if (!rule.getRemoteSubnets().empty())
        os << ",remoteSubnets=" << rule.getRemoteSubnets();
    if (rule.getRedirectDestGrpURI())
        os << ",redirectGroup=" << rule.getRedirectDestGrpURI().get();

    os << "]";
    return os;
}

bool operator==(const PolicyRedirectDest& lhs,
                const PolicyRedirectDest& rhs) {
    return ((lhs.getIp()==rhs.getIp()) &&
            (lhs.getMac()==rhs.getMac()) &&
            (lhs.getRD()->getURI()==rhs.getRD()->getURI()) &&
            (lhs.getBD()->getURI()==rhs.getBD()->getURI()));
}

bool operator!=(const PolicyRedirectDest& lhs, const PolicyRedirectDest& rhs) {
    return !operator==(lhs,rhs);
}

bool PolicyManager::getPolicyDestGroup(URI redirURI,
                                       redir_dest_list_t &redirList,
                             uint8_t &hashParam_, uint8_t &hashOpt_)
{
    lock_guard<mutex> guard(state_mutex);
    redir_dst_grp_map_t::iterator it = redirGrpMap.find(redirURI);
    if(it == redirGrpMap.end()){
        return false;
    }
    hashParam_ = it->second.resilientHashEnabled;
    hashOpt_ = it->second.hashAlgo;
    redirList.insert(redirList.end(),
                     it->second.redirDstList.begin(),
                     it->second.redirDstList.end());
    return true;
}

static bool compareRedirects(const shared_ptr<PolicyRedirectDest>& lhs,
                             const shared_ptr<PolicyRedirectDest>& rhs)
{
    return (lhs->getIp() < rhs->getIp())?true:false;
}

void PolicyManager::updateRedirectDestGroup(const URI& uri,
                                            uri_set_t &notifyGroup) {
    using namespace modelgbp::gbp;
    using namespace modelgbp::gbpe;
    typedef shared_ptr<RedirectDestToDomainRSrc> redir_domp_t;
    boost::optional<shared_ptr<RedirectDestGroup>> redirDstGrp =
    RedirectDestGroup::resolve(framework,uri);
    RedirectDestGrpState &redirState = redirGrpMap[uri];
    if(!redirDstGrp) {
        notifyGroup.insert(redirState.ctrctSet.begin(),
                           redirState.ctrctSet.end());
        redirGrpMap.erase(uri);
        return;
    }
    std::vector<shared_ptr<RedirectDest>> redirDests;
    redirDstGrp.get()->resolveGbpRedirectDest(redirDests);
    PolicyManager::redir_dest_list_t newRedirDests;

    LOG(DEBUG) << uri;
    for(shared_ptr<RedirectDest>& redirDest : redirDests) {
        /*Redirect Destination should be completely resolved
         in order to be useful for forwarding*/
        std::vector<redir_domp_t> redirDoms;
        redirDest->resolveGbpRedirectDestToDomainRSrc(redirDoms);
        boost::optional<shared_ptr<BridgeDomain>> bd;
        boost::optional<shared_ptr<RoutingDomain>> rd;
        boost::optional<shared_ptr<InstContext>> bdInst,rdInst;
        for(const redir_domp_t &redirDom: redirDoms) {
            if(!redirDom->getTargetURI() || !redirDom->getTargetClass())
                continue;
            class_id_t redirDomClass = redirDom->getTargetClass().get();
            if(redirDomClass == BridgeDomain::CLASS_ID) {
                bd = BridgeDomain::resolve(framework,
                                           redirDom->getTargetURI().get());
                if(!bd) {
                    break;
		}
                bdInst = bd.get()->resolveGbpeInstContext();
            }
            if(redirDomClass == RoutingDomain::CLASS_ID) {
                rd = RoutingDomain::resolve(framework,
                                            redirDom->getTargetURI().get());
                if(!rd){
                   break;
                }
                rdInst = rd.get()->resolveGbpeInstContext();
            }
        }
        if(!bdInst || !rdInst || !redirDest->isIpSet() ||
           !redirDest->isMacSet()) {
            continue;
        }
        boost::system::error_code ec;
        boost::asio::ip::address addr = address::from_string(
                                            redirDest->getIp().get(),ec);
        if(ec) {
            continue;
        }
        newRedirDests.push_back(
            make_shared<PolicyRedirectDest>(redirDest,addr,
                                            redirDest->getMac().get(),
                                            rd.get(), bd.get(),
                                            rdInst.get(), bdInst.get()));
    }
    redir_dest_list_t::const_iterator li = redirState.redirDstList.begin();
    redir_dest_list_t::const_iterator ri = newRedirDests.begin();
    while ((li != redirState.redirDstList.end()) &&
           (ri != newRedirDests.end()) &&
           (li->get() == ri->get())) {
        ++li;
        ++ri;
    }
    if((li != redirState.redirDstList.end()) ||
       (ri != newRedirDests.end()) ||
       (redirDstGrp.get()->getHashAlgo(HashingAlgorithmEnumT::CONST_SYMMETRIC)
        != redirState.hashAlgo) ||
       (redirDstGrp.get()->getResilientHashEnabled(1) != redirState.resilientHashEnabled)) {
        notifyGroup.insert(redirState.ctrctSet.begin(),
                           redirState.ctrctSet.end());
    }
    /* Order in which the next-hops are inserted may not be the order of
     * resolution. Return in ascending order
     */
    newRedirDests.sort(compareRedirects);
    redirState.redirDstList.swap(newRedirDests);
    redirState.hashAlgo = redirDstGrp.get()->getHashAlgo(
                             HashingAlgorithmEnumT::CONST_SYMMETRIC);
    redirState.resilientHashEnabled = redirDstGrp.get()->getResilientHashEnabled(1);
}

void PolicyManager::updateRedirectDestGroups(uri_set_t &notifyGroup) {
    for (PolicyManager::redir_dst_grp_map_t::iterator itr =
         redirGrpMap.begin(); itr != redirGrpMap.end(); itr++) {
        updateRedirectDestGroup(itr->first, notifyGroup);
    }
}

void PolicyManager::resolveSubnets(OFFramework& framework,
                                   const optional<URI>& subnets_uri,
                                   /* out */ network::subnets_t& subnets_out) {
    using modelgbp::gbp::Subnets;
    using modelgbp::gbp::Subnet;

    if (!subnets_uri) return;
    optional<shared_ptr<Subnets> > subnets_obj =
        Subnets::resolve(framework, subnets_uri.get());
    if (!subnets_obj) return;

    vector<shared_ptr<Subnet> > subnets;
    subnets_obj.get()->resolveGbpSubnet(subnets);

    boost::system::error_code ec;

    for (shared_ptr<Subnet>& subnet : subnets) {
        if (!subnet->isAddressSet() || !subnet->isPrefixLenSet())
            continue;
        address addr = address::from_string(subnet->getAddress().get(), ec);
        if (ec) continue;
        addr = network::mask_address(addr, subnet->getPrefixLen().get());
        subnets_out.insert(make_pair(addr.to_string(),
                                     subnet->getPrefixLen().get()));
    }
}

template <typename Parent, typename Child>
void resolveChildren(shared_ptr<Parent>& parent,
                     /* out */ vector<shared_ptr<Child> > &children) { }
template <>
void resolveChildren(shared_ptr<modelgbp::gbp::Subject>& subject,
                     vector<shared_ptr<modelgbp::gbp::Rule> > &rules) {
    subject->resolveGbpRule(rules);
}
template <>
void resolveChildren(shared_ptr<modelgbp::gbp::SecGroupSubject>& subject,
                     vector<shared_ptr<modelgbp::gbp::SecGroupRule> > &rules) {
    subject->resolveGbpSecGroupRule(rules);
}
template <>
void resolveChildren(shared_ptr<modelgbp::gbp::Contract>& contract,
                     vector<shared_ptr<modelgbp::gbp::Subject> > &subjects) {
    contract->resolveGbpSubject(subjects);
}
template <>
void resolveChildren(shared_ptr<modelgbp::gbp::SecGroup>& secgroup,
                     vector<shared_ptr<modelgbp::gbp::SecGroupSubject> > &subjects) {
    secgroup->resolveGbpSecGroupSubject(subjects);
}

template <typename Rule>
void resolveRemoteSubnets(OFFramework& framework,
                          shared_ptr<Rule>& parent,
                          /* out */ network::subnets_t &remoteSubnets) {}

template <>
void resolveRemoteSubnets(OFFramework& framework,
                          shared_ptr<modelgbp::gbp::SecGroupRule>& rule,
                          /* out */ network::subnets_t &remoteSubnets) {
    typedef modelgbp::gbp::SecGroupRuleToRemoteAddressRSrc RASrc;
    vector<shared_ptr<RASrc> > raSrcs;
    rule->resolveGbpSecGroupRuleToRemoteAddressRSrc(raSrcs);
    for (const shared_ptr<RASrc>& ra : raSrcs) {
        optional<URI> subnets_uri = ra->getTargetURI();
        PolicyManager::resolveSubnets(framework, subnets_uri, remoteSubnets);
    }
}

template <typename Parent, typename Subject, typename Rule>
static bool updatePolicyRules(OFFramework& framework,
                              const URI& parentURI, bool& notFound,
                              PolicyManager::rule_list_t& oldRules,
                              PolicyManager::uri_set_t &oldRedirGrps,
                              PolicyManager::uri_set_t &newRedirGrps)
{
    using modelgbp::gbpe::L24Classifier;
    using modelgbp::gbp::RuleToClassifierRSrc;
    using modelgbp::gbp::RuleToActionRSrc;
    using modelgbp::gbp::AllowDenyAction;
    using modelgbp::gbp::RedirectAction;
    using modelgbp::gbp::RedirectDestGroup;

    optional<shared_ptr<Parent> > parent =
        Parent::resolve(framework, parentURI);
    if (!parent) {
        notFound = true;
        return false;
    }
    notFound = false;

    /* get all classifiers for this parent as an ordered-list */
    PolicyManager::rule_list_t newRules;
    OrderComparator<shared_ptr<Rule> > ruleComp;
    OrderComparator<shared_ptr<L24Classifier> > classifierComp;
    vector<shared_ptr<Subject> > subjects;
    resolveChildren(parent.get(), subjects);
    for (shared_ptr<Subject>& sub : subjects) {
        vector<shared_ptr<Rule> > rules;
        resolveChildren(sub, rules);
        stable_sort(rules.begin(), rules.end(), ruleComp);

        uint16_t rulePrio = PolicyManager::MAX_POLICY_RULE_PRIORITY;

        for (shared_ptr<Rule>& rule : rules) {
            if (!rule->isDirectionSet()) {
                continue;       // ignore rules with no direction
            }
            uint8_t dir = rule->getDirection().get();
            network::subnets_t remoteSubnets;
            resolveRemoteSubnets(framework, rule, remoteSubnets);
            vector<shared_ptr<L24Classifier> > classifiers;
            vector<shared_ptr<RuleToClassifierRSrc> > clsRel;
            rule->resolveGbpRuleToClassifierRSrc(clsRel);

            for (shared_ptr<RuleToClassifierRSrc>& r : clsRel) {
                if (!r->isTargetSet() ||
                    r->getTargetClass().get() != L24Classifier::CLASS_ID) {
                    continue;
                }
                optional<shared_ptr<L24Classifier> > cls =
                    L24Classifier::resolve(framework, r->getTargetURI().get());
                if (cls) {
                    classifiers.push_back(cls.get());
                }
            }
            stable_sort(classifiers.begin(), classifiers.end(), classifierComp);

            vector<shared_ptr<RuleToActionRSrc> > actRel;
            rule->resolveGbpRuleToActionRSrc(actRel);
            bool ruleAllow = true;
            bool ruleRedirect = false;
            uint32_t minOrder = UINT32_MAX;
            optional<shared_ptr<RedirectDestGroup>> redirDstGrp;
            optional<URI> destGrpUri;
            for (shared_ptr<RuleToActionRSrc>& r : actRel) {
                if (!r->isTargetSet()) {
                    continue;
                }
                if(r->getTargetClass().get() == AllowDenyAction::CLASS_ID) {
                    optional<shared_ptr<AllowDenyAction> > act =
                        AllowDenyAction::resolve(framework, r->getTargetURI().get());
                    if (act) {
                        if (act.get()->getOrder(UINT32_MAX-1) < minOrder) {
                            minOrder = act.get()->getOrder(UINT32_MAX-1);
                            ruleAllow = act.get()->getAllow(0) != 0;
                        }
                    }
                }
                else if(r->getTargetClass().get() ==
                        RedirectAction::CLASS_ID) {
                    optional<shared_ptr<RedirectAction> > act =
                    RedirectAction::resolve(framework, r->getTargetURI().get());
                    ruleRedirect = true;
                    ruleAllow = false;
                    if (!act) {
                        continue;
                    }
                    optional<shared_ptr<modelgbp::gbp::RedirectActionToDestGrpRSrc>>
                        destRef = act.get()->resolveGbpRedirectActionToDestGrpRSrc();
                    if(!destRef){
                        continue;
                    }
                    destGrpUri = destRef.get()->getTargetURI();
                    if(!destGrpUri) {
                        continue;
                    }
                    redirDstGrp =
                    RedirectDestGroup::resolve(framework, destGrpUri.get());
                    newRedirGrps.insert(destGrpUri.get());
                }
            }

            uint16_t clsPrio = 0;
            for (const shared_ptr<L24Classifier>& c : classifiers) {
                newRules.push_back(std::
                                   make_shared<PolicyRule>(dir,
                                                           rulePrio - clsPrio,
                                                           c, ruleAllow,
                                                           remoteSubnets,
                                                           ruleRedirect,
                                                           destGrpUri));
                if (clsPrio < 127)
                    clsPrio += 1;
            }
            if (rulePrio > 128)
                rulePrio -= 128;
        }
    }
    PolicyManager::rule_list_t::const_iterator oi = oldRules.begin();
    while(oi != oldRules.end()) {
        if(oi->get()->getRedirectDestGrpURI()) {
            oldRedirGrps.insert(oi->get()->getRedirectDestGrpURI().get());
        }
        ++oi;
    }
    PolicyManager::rule_list_t::const_iterator li = oldRules.begin();
    PolicyManager::rule_list_t::const_iterator ri = newRules.begin();
    while (li != oldRules.end() && ri != newRules.end() &&
           li->get() == ri->get()) {
        ++li;
        ++ri;
    }
    bool updated = (li != oldRules.end() || ri != newRules.end());
    if (updated) {
        oldRules.swap(newRules);
        for (shared_ptr<PolicyRule>& c : oldRules) {
            LOG(DEBUG) << parentURI << ": " << *c;
        }
    }
    return updated;
}

bool PolicyManager::updateSecGrpRules(const URI& secGrpURI, bool& notFound) {
    using namespace modelgbp::gbp;
    uri_set_t oldRedirGrps, newRedirGrps;
    return updatePolicyRules<SecGroup, SecGroupSubject,
                             SecGroupRule>(framework, secGrpURI,
                                           notFound, secGrpMap[secGrpURI],
                                           oldRedirGrps, newRedirGrps);
}

bool PolicyManager::updateContractRules(const URI& contrURI, bool& notFound) {
    using namespace modelgbp::gbp;
    uri_set_t oldRedirGrps, newRedirGrps;
    ContractState& cs = contractMap[contrURI];
    bool updated = updatePolicyRules<Contract, Subject,
                                     Rule>(framework, contrURI,
                                           notFound, cs.rules,
                                           oldRedirGrps,
                                           newRedirGrps);
    for (const URI& u : oldRedirGrps) {
        if(redirGrpMap.find(u) != redirGrpMap.end()) {
            redirGrpMap[u].ctrctSet.erase(contrURI);
        }
    }
    for (const URI& u : newRedirGrps) {
        redirGrpMap[u].ctrctSet.insert(contrURI);
    }
    return updated;
}

void PolicyManager::updateContracts() {
    unique_lock<mutex> guard(state_mutex);
    uri_set_t contractsToNotify;

    /* recompute the rules for all contracts if a policy
       object changed */
    for (PolicyManager::contract_map_t::iterator itr =
             contractMap.begin();
         itr != contractMap.end();) {

        bool notFound = false;
        if (updateContractRules(itr->first, notFound)) {
            contractsToNotify.insert(itr->first);
        }
        /*
         * notFound == true may happen if the contract was
         * removed or there is a reference from a group to
         * a contract that has not been received yet.
         */
        if (notFound) {
            contractsToNotify.insert(itr->first);
            // if contract has providers/consumers, only
            // clear the rules
            if (itr->second.providerGroups.empty() &&
                itr->second.consumerGroups.empty() &&
                itr->second.intraGroups.empty()) {
                itr = contractMap.erase(itr);
            } else {
                itr->second.rules.clear();
                ++itr;
            }
        } else {
            ++itr;
        }
    }
    guard.unlock();

    for (const URI& u : contractsToNotify) {
        notifyContract(u);
    }
}

void PolicyManager::updateSecGrps() {
    /* recompute the rules for all security groups if a policy
       object changed */
    unique_lock<mutex> guard(state_mutex);

    uri_set_t toNotify;
    PolicyManager::secgrp_map_t::iterator it =
        secGrpMap.begin();
    while (it != secGrpMap.end()) {
        bool notfound = false;
        if (updateSecGrpRules(it->first, notfound)) {
            toNotify.insert(it->first);
        }
        if (notfound) {
            toNotify.insert(it->first);
            it = secGrpMap.erase(it);
        } else {
            ++it;
        }
    }
    guard.unlock();

    for (const URI& u : toNotify) {
        notifySecGroup(u);
    }
}

void PolicyManager::getContractProviders(const URI& contractURI,
                                         /* out */ uri_set_t& epgURIs) {
    lock_guard<mutex> guard(state_mutex);
    contract_map_t::const_iterator it = contractMap.find(contractURI);
    if (it != contractMap.end()) {
        epgURIs.insert(it->second.providerGroups.begin(),
                       it->second.providerGroups.end());
    }
}

void PolicyManager::getContractConsumers(const URI& contractURI,
                                         /* out */ uri_set_t& epgURIs) {
    lock_guard<mutex> guard(state_mutex);
    contract_map_t::const_iterator it = contractMap.find(contractURI);
    if (it != contractMap.end()) {
        epgURIs.insert(it->second.consumerGroups.begin(),
                       it->second.consumerGroups.end());
    }
}

void PolicyManager::getContractIntra(const URI& contractURI,
                                         /* out */ uri_set_t& epgURIs) {
    lock_guard<mutex> guard(state_mutex);
    contract_map_t::const_iterator it = contractMap.find(contractURI);
    if (it != contractMap.end()) {
        epgURIs.insert(it->second.intraGroups.begin(),
                       it->second.intraGroups.end());
    }
}

void PolicyManager::getContractsForGroup(const URI& eg,
                                         /* out */ uri_set_t& contractURIs) {
    using namespace modelgbp::gbp;
    optional<shared_ptr<EpGroup> > epg = EpGroup::resolve(framework, eg);
    if (!epg) return;

    vector<shared_ptr<EpGroupToProvContractRSrc> > provRel;
    epg.get()->resolveGbpEpGroupToProvContractRSrc(provRel);
    vector<shared_ptr<EpGroupToConsContractRSrc> > consRel;
    epg.get()->resolveGbpEpGroupToConsContractRSrc(consRel);
    vector<shared_ptr<EpGroupToIntraContractRSrc> > intraRel;
    epg.get()->resolveGbpEpGroupToIntraContractRSrc(intraRel);

    for (shared_ptr<EpGroupToProvContractRSrc>& rel : provRel) {
        if (rel->isTargetSet()) {
            contractURIs.insert(rel->getTargetURI().get());
        }
    }
    for (shared_ptr<EpGroupToConsContractRSrc>& rel : consRel) {
        if (rel->isTargetSet()) {
            contractURIs.insert(rel->getTargetURI().get());
        }
    }
    for (shared_ptr<EpGroupToIntraContractRSrc>& rel : intraRel) {
        if (rel->isTargetSet()) {
            contractURIs.insert(rel->getTargetURI().get());
        }
    }
}

void PolicyManager::getContractRules(const URI& contractURI,
                                     /* out */ rule_list_t& rules) {
    lock_guard<mutex> guard(state_mutex);
    contract_map_t::const_iterator it = contractMap.find(contractURI);
    if (it != contractMap.end()) {
        rules.insert(rules.end(), it->second.rules.begin(),
                     it->second.rules.end());
    }
}

void PolicyManager::getSecGroupRules(const URI& secGroupURI,
                                     /* out */ rule_list_t& rules) {
    lock_guard<mutex> guard(state_mutex);
    secgrp_map_t::const_iterator it = secGrpMap.find(secGroupURI);
    if (it != secGrpMap.end()) {
        rules.insert(rules.end(), it->second.begin(), it->second.end());
    }
}

bool PolicyManager::contractExists(const opflex::modb::URI& cURI) {
    lock_guard<mutex> guard(state_mutex);
    return contractMap.find(cURI) != contractMap.end();
}

void PolicyManager::updateL3Nets(const opflex::modb::URI& rdURI,
                                 uri_set_t& contractsToNotify) {
    using namespace modelgbp::gbp;
    RoutingDomainState& rds = rd_map[rdURI];
    optional<shared_ptr<RoutingDomain > > rd =
        RoutingDomain::resolve(framework, rdURI);

    if (rd) {
        vector<shared_ptr<L3ExternalDomain> > extDoms;
        vector<shared_ptr<L3ExternalNetwork> > extNets;
        rd.get()->resolveGbpL3ExternalDomain(extDoms);
        for (shared_ptr<L3ExternalDomain>& extDom : extDoms) {
            extDom->resolveGbpL3ExternalNetwork(extNets);
        }

        unordered_set<URI> newNets;
        for (shared_ptr<L3ExternalNetwork> net : extNets) {
            newNets.insert(net->getURI());

            L3NetworkState& l3s = l3n_map[net->getURI()];
            if (l3s.routingDomain && l3s.natEpg) {
                uri_ref_map_t::iterator it =
                    nat_epg_l3_ext.find(l3s.natEpg.get());
                if (it != nat_epg_l3_ext.end()) {
                    it->second.erase(net->getURI());
                    if (it->second.size() == 0)
                        nat_epg_l3_ext.erase(it);
                }
            }

            l3s.routingDomain = rd;

            optional<shared_ptr<L3ExternalNetworkToNatEPGroupRSrc> > natRef =
                net->resolveGbpL3ExternalNetworkToNatEPGroupRSrc();
            if (natRef) {
                optional<URI> natEpg = natRef.get()->getTargetURI();
                if (natEpg) {
                    l3s.natEpg = natEpg.get();
                    uri_set_t& s = nat_epg_l3_ext[l3s.natEpg.get()];
                    s.insert(net->getURI());
                }
            } else {
                l3s.natEpg = boost::none;
            }

            updateGroupContracts(L3ExternalNetwork::CLASS_ID,
                                 net->getURI(), contractsToNotify);
        }
        for (const URI& net : rds.extNets) {
            if (newNets.find(net) == newNets.end()) {
                l3n_map_t::iterator lit = l3n_map.find(net);
                if (lit != l3n_map.end()) {
                    if (lit->second.natEpg) {
                        uri_ref_map_t::iterator git =
                            nat_epg_l3_ext.find(lit->second.natEpg.get());
                        if (git != nat_epg_l3_ext.end()) {
                            git->second.erase(net);
                            if (git->second.size() == 0)
                                nat_epg_l3_ext.erase(git);
                        }
                    }
                }

                l3n_map.erase(lit);

                updateGroupContracts(L3ExternalNetwork::CLASS_ID,
                                     net, contractsToNotify);
            }
        }
        rds.extNets = newNets;
    } else {
        for (const URI& net : rds.extNets) {
            l3n_map.erase(net);
            updateGroupContracts(L3ExternalNetwork::CLASS_ID,
                                 net, contractsToNotify);
        }
        rd_map.erase(rdURI);
    }
}

uint8_t PolicyManager::getEffectiveRoutingMode(const URI& egURI) {
    using namespace modelgbp::gbp;

    optional<shared_ptr<BridgeDomain> > bd = getBDForGroup(egURI);

    uint8_t routingMode = RoutingModeEnumT::CONST_ENABLED;
    if (bd)
        routingMode = bd.get()->getRoutingMode(routingMode);

    return routingMode;
}

boost::optional<address>
PolicyManager::getRouterIpForSubnet(modelgbp::gbp::Subnet& subnet) {
    optional<const string&> routerIpStr = subnet.getVirtualRouterIp();
    if (routerIpStr) {
        boost::system::error_code ec;
        address routerIp = address::from_string(routerIpStr.get(), ec);
        if (ec) {
            LOG(WARNING) << "Invalid router IP for subnet "
                         << subnet.getURI() << ": "
                         << routerIpStr.get() << ": " << ec.message();
        } else {
            return routerIp;
        }
    }
    return boost::none;
}

void PolicyManager::updateDomain(class_id_t class_id, const URI& uri) {
    using namespace modelgbp::gbp;
    unique_lock<mutex> guard(state_mutex);

    uri_set_t notifyGroups;
    uri_set_t notifyRds;

    if (class_id == modelgbp::gbp::EpGroup::CLASS_ID) {
        group_map[uri];
    }
    for (PolicyManager::group_map_t::iterator itr = group_map.begin();
         itr != group_map.end(); ) {
        bool toRemove = false;
        if (updateEPGDomains(itr->first, toRemove)) {
            notifyGroups.insert(itr->first);
        }
        itr = (toRemove ? group_map.erase(itr) : ++itr);
    }
    // Determine routing-domains that may be affected by changes to NAT EPG
    for (const URI& u : notifyGroups) {
        uri_ref_map_t::iterator it = nat_epg_l3_ext.find(u);
        if (it != nat_epg_l3_ext.end()) {
            for (const URI& extNet : it->second) {
                l3n_map_t::iterator it2 = l3n_map.find(extNet);
                if (it2 != l3n_map.end()) {
                    notifyRds.insert(it2->second.routingDomain.get()->getURI());
                }
            }
        }
    }
    notifyRds.erase(uri);   // Avoid updating twice
    guard.unlock();

    for (const URI& u : notifyGroups) {
        notifyEPGDomain(u);
    }
    if (class_id != modelgbp::gbp::EpGroup::CLASS_ID) {
        notifyDomain(class_id, uri);
    }
    for (const URI& rd : notifyRds) {
        notifyDomain(RoutingDomain::CLASS_ID, rd);
    }
}

PolicyManager::DomainListener::DomainListener(PolicyManager& pmanager_)
    : pmanager(pmanager_) {}
PolicyManager::DomainListener::~DomainListener() {}

void PolicyManager::DomainListener::objectUpdated(class_id_t class_id,
                                                  const URI& uri) {
    pmanager.taskQueue.dispatch("dl"+uri.toString(), [=]() {
            pmanager.updateDomain(class_id, uri);
        });
}

void PolicyManager::
executeAndNotifyContract(const std::function<void(uri_set_t&)>& func) {
    uri_set_t contractsToNotify;

    {
        unique_lock<mutex> guard(state_mutex);
        func(contractsToNotify);
    }

    for (const URI& u : contractsToNotify) {
        notifyContract(u);
    }
}

PolicyManager::ContractListener::ContractListener(PolicyManager& pmanager_)
    : pmanager(pmanager_) {}

PolicyManager::ContractListener::~ContractListener() {}

void PolicyManager::ContractListener::objectUpdated(class_id_t classId,
                                                    const URI& uri) {
    using namespace modelgbp::gbp;
    LOG(DEBUG) << "ContractListener update for URI " << uri;

    if (classId == EpGroup::CLASS_ID ||
        classId == L3ExternalNetwork::CLASS_ID) {
        pmanager.taskQueue.dispatch("cl"+uri.toString(), [=]() {
                pmanager.executeAndNotifyContract([&](uri_set_t& notif) {
                        pmanager.updateGroupContracts(classId, uri, notif);
                    });
            });
    } else if (classId == RoutingDomain::CLASS_ID) {
        pmanager.taskQueue.dispatch("cl"+uri.toString(), [=]() {
                pmanager.executeAndNotifyContract([&](uri_set_t& notif) {
                        pmanager.updateL3Nets(uri, notif);
                    });
            });
    } else if (classId == RedirectDestGroup::CLASS_ID) {
        pmanager.taskQueue.dispatch("cl"+uri.toString(), [=]() {
            pmanager.executeAndNotifyContract([&](uri_set_t& notif) {
                pmanager.updateRedirectDestGroup(uri, notif);
            });
        });
    } else if (classId == RedirectDest::CLASS_ID) {
        pmanager.taskQueue.dispatch("cl"+uri.toString(), [=]() {
            pmanager.executeAndNotifyContract([&](uri_set_t& notif) {
                pmanager.updateRedirectDestGroups(notif);
            });
        });
    } else {
        {
            unique_lock<mutex> guard(pmanager.state_mutex);
            if (classId == Contract::CLASS_ID) {
                pmanager.contractMap[uri];
            }
        }

        pmanager.taskQueue.dispatch("contract", [this]() {
                pmanager.updateContracts();
            });
    }
}

PolicyManager::SecGroupListener::SecGroupListener(PolicyManager& pmanager_)
    : pmanager(pmanager_) {}

PolicyManager::SecGroupListener::~SecGroupListener() {}

void PolicyManager::SecGroupListener::objectUpdated(class_id_t classId,
                                                    const URI& uri) {
    LOG(DEBUG) << "SecGroupListener update for URI " << uri;
    {
        unique_lock<mutex> guard(pmanager.state_mutex);
        if (classId == modelgbp::gbp::SecGroup::CLASS_ID) {
            pmanager.secGrpMap[uri];
        }
    }

    pmanager.taskQueue.dispatch("secgroup", [this]() {
            pmanager.updateSecGrps();
        });
}

PolicyManager::ConfigListener::ConfigListener(PolicyManager& pmanager_)
    : pmanager(pmanager_) {}

PolicyManager::ConfigListener::~ConfigListener() {}

void PolicyManager::ConfigListener::objectUpdated(class_id_t, const URI& uri) {
    pmanager.notifyConfig(uri);
}

} /* namespace opflexagent */