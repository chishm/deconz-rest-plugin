/*
 * Copyright (c) 2021 dresden elektronik ingenieurtechnik gmbh.
 * All rights reserved.
 *
 * The software in this package is published under the terms of the BSD
 * style license a copy of which has been included with this distribution in
 * the LICENSE.txt file.
 *
 */

#include <QBasicTimer>
#include <QElapsedTimer>
#include <QTimerEvent>
#include <array>
#include <tuple>
#include <deconz/dbg_trace.h>
#include <deconz/node.h>
#include "device.h"
#include "device_access_fn.h"
#include "device_descriptions.h"
#include "event.h"
#include "utils/utils.h"
#include "zcl/zcl.h"
#include "zdp.h"

#define STATE_LEVEL_BINDING  StateLevel1
#define STATE_LEVEL_POLL     StateLevel2

typedef void (*DeviceStateHandler)(Device *, const Event &);

/*! Device state machine description can be found in the wiki:

       https://github.com/dresden-elektronik/deconz-rest-plugin-v2/wiki/Device-Class#state-machine
*/
void DEV_InitStateHandler(Device *device, const Event &event);
void DEV_IdleStateHandler(Device *device, const Event &event);
void DEV_NodeDescriptorStateHandler(Device *device, const Event &event);
void DEV_ActiveEndpointsStateHandler(Device *device, const Event &event);
void DEV_SimpleDescriptorStateHandler(Device *device, const Event &event);
void DEV_BasicClusterStateHandler(Device *device, const Event &event);
void DEV_GetDeviceDescriptionHandler(Device *device, const Event &event);
void DEV_BindingHandler(Device *device, const Event &event);
void DEV_BindingTableVerifyHandler(Device *device, const Event &event);
void DEV_PollIdleStateHandler(Device *device, const Event &event);
void DEV_PollNextStateHandler(Device *device, const Event &event);
void DEV_PollBusyStateHandler(Device *device, const Event &event);
void DEV_DeadStateHandler(Device *device, const Event &event);

// enable domain specific string literals
using namespace deCONZ::literals;

constexpr int MinMacPollRxOn = 8000; // 7680 ms + some space for timeout
constexpr int MaxPollItemRetries = 3;

struct PollItem
{
    explicit PollItem(const Resource *r, const ResourceItem *i, const QVariant &p) :
        resource(r), item(i), readParameters(p) {}
    size_t retry = 0;
    const Resource *resource = nullptr;
    const ResourceItem *item = nullptr;
    QVariant readParameters;
};

struct BindingContext
{
    QElapsedTimer bindingVerify; //! time to track last binding table verification
    size_t bindingIter = 0;
    bool mgmtBindSupported = false;
};

class DevicePrivate
{
public:
    void setState(DeviceStateHandler newState, DEV_StateLevel level = StateLevel0);
    void startStateTimer(int IntervalMs, DEV_StateLevel level);
    void stopStateTimer(DEV_StateLevel level);

    Device *q = nullptr; //! reference to public interface
    deCONZ::ApsController *apsCtrl = nullptr; //! opaque instance pointer forwarded to external functions

    /*! sub-devices are not yet referenced via pointers since these may become dangling.
        This is a helper to query the actual sub-device Resource* on demand.

        {uniqueid, (RSensors | RLights)}
    */
    std::vector<std::tuple<QString, const char*>> subDevices;
    const deCONZ::Node *node = nullptr; //! a reference to the deCONZ core node
    DeviceKey deviceKey = 0; //! for physical devices this is the MAC address

    /*! The currently active state handler function(s).
        Indexes >0 represent sub states of StateLevel0 running in parallel.
    */
    std::array<DeviceStateHandler, StateLevelMax> state{0};

    std::array<QBasicTimer, StateLevelMax> timer; //! internal single shot timer one for each state level
    QElapsedTimer awake; //! time to track when an end-device was last awake
    BindingContext binding; //! only used by binding sub state machine
    std::vector<PollItem> pollItems; //! queue of items to poll
    bool managed = false; //! a managed device doesn't rely on legacy implementation of polling etc.
    ZDP_Result zdpResult; //! keep track of a running ZDP request
    DA_ReadResult readResult; //! keep track of a running "read" request
};

void DEV_EnqueueEvent(Device *device, const char *event)
{
    Q_ASSERT(device);
    Q_ASSERT(event);
    emit device->eventNotify(Event(device->prefix(), event, 0, device->key()));
}

Resource *DEV_GetSubDevice(Device *device, const char *prefix, const QString &identifier)
{
    for (auto &sub : device->subDevices())
    {
        if (prefix && sub->prefix() != prefix)
        {
            continue;
        }

        if (sub->item(RAttrUniqueId)->toString() == identifier || sub->item(RAttrId)->toString() == identifier)
        {
            return sub;
        }
    }

    return nullptr;
}

void DEV_InitStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() != RAttrLastSeen)
    {
        DBG_Printf(DBG_INFO, "DEV Init event %s/0x%016llX/%s\n", event.resource(), event.deviceKey(), event.what());
    }

    if (event.what() == REventStateEnter)
    {
        d->zdpResult = { };

        if ((event.deviceKey() & 0x00212E0000000000LLU) == 0x00212E0000000000LLU)
        {
            d->node = DEV_GetCoreNode(device->key());
            if (d->node && d->node->address().nwk() == 0x0000)
            {
                d->setState(DEV_DeadStateHandler);
                return; // ignore coordinaor for now
            }
        }
    }

    if (event.what() == REventPoll ||
        event.what() == REventAwake ||
        event.what() == RConfigReachable ||
        event.what() == RStateReachable ||
        event.what() == REventStateTimeout ||
        event.what() == RStateLastUpdated)
    {
        // lazy reference to deCONZ::Node
        if (!device->node())
        {
            d->node = DEV_GetCoreNode(device->key());
        }

        if (device->node())
        {
            device->item(RAttrExtAddress)->setValue(device->node()->address().ext());
            device->item(RAttrNwkAddress)->setValue(device->node()->address().nwk());

            // got a node, jump to verification
            if (!device->node()->nodeDescriptor().isNull() || device->reachable())
            {
                d->setState(DEV_NodeDescriptorStateHandler);
            }
        }
        else
        {
            DBG_Printf(DBG_INFO, "DEV Init no node found: 0x%016llX\n", event.deviceKey());

            if ((device->key() & 0xffffffff00000000LLU) == 0)
            {
                d->setState(DEV_DeadStateHandler);
                return; // ignore ZGP for now
            }
        }
    }
}

void DEV_CheckItemChanges(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;
    std::vector<Resource*> subDevices;

    if (event.what() == REventAwake || event.what() == REventPoll)
    {
        subDevices = device->subDevices();
    }
    else
    {
        auto *sub = DEV_GetSubDevice(device, event.resource(), event.id());
        if (sub)
        {
            subDevices.push_back(sub);
        }
    }

    for (auto *sub : subDevices)
    {
        if (sub && !sub->stateChanges().empty())
        {
            auto *item = sub->item(event.what());
            for (auto &change : sub->stateChanges())
            {
                if (item)
                {
                    change.verifyItemChange(item);
                }
                change.tick(sub, d->apsCtrl);
            }

            sub->cleanupStateChanges();
        }
    }
}

/*! #2 This state checks that a valid NodeDescriptor is available.
 */
void DEV_NodeDescriptorStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        if (!device->node()->nodeDescriptor().isNull())
        {
            DBG_Printf(DBG_INFO, "ZDP node descriptor verified: 0x%016llX\n", device->key());
            d->setState(DEV_ActiveEndpointsStateHandler);
        }
        else if (!device->reachable()) // can't be queried, go back to #1 init
        {
            d->setState(DEV_InitStateHandler);
        }
        else
        {
            d->zdpResult = ZDP_NodeDescriptorReq(device->item(RAttrNwkAddress)->toNumber(), d->apsCtrl);
            if (d->zdpResult.isEnqueued)
            {
                d->startStateTimer(MinMacPollRxOn, StateLevel0);
            }
            else
            {
                d->setState(DEV_InitStateHandler);
            }
        }
    }
    else if (event.what() == REventApsConfirm)
    {
        Q_ASSERT(event.deviceKey() == device->key());
        if (d->zdpResult.apsReqId == EventApsConfirmId(event) && EventApsConfirmStatus(event) != deCONZ::ApsSuccessStatus)
        {
            d->setState(DEV_InitStateHandler);
        }
    }
    else if (event.what() == REventNodeDescriptor) // received the node descriptor
    {
        d->stopStateTimer(StateLevel0);
        d->setState(DEV_InitStateHandler); // evaluate egain from state #1 init
        DEV_EnqueueEvent(device, REventAwake);
    }
    else if (event.what() == REventStateTimeout)
    {
        DBG_Printf(DBG_INFO, "read ZDP node descriptor timeout: 0x%016llX\n", device->key());
        d->setState(DEV_InitStateHandler);
    }
}

/*! #3 This state checks that active endpoints are known.
 */
void DEV_ActiveEndpointsStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        if (!device->node()->endpoints().empty())
        {
            DBG_Printf(DBG_INFO, "ZDP active endpoints verified: 0x%016llX\n", device->key());
            d->setState(DEV_SimpleDescriptorStateHandler);
        }
        else if (!device->reachable())
        {
            d->setState(DEV_InitStateHandler);
        }
        else
        {
            d->zdpResult = ZDP_ActiveEndpointsReq(device->item(RAttrNwkAddress)->toNumber(), d->apsCtrl);
            if (d->zdpResult.isEnqueued)
            {
                d->startStateTimer(MinMacPollRxOn, StateLevel0);
            }
            else
            {
                d->setState(DEV_InitStateHandler);
            }
        }
    }
    else if (event.what() == REventApsConfirm)
    {
        Q_ASSERT(event.deviceKey() == device->key());
        if (d->zdpResult.apsReqId == EventApsConfirmId(event) && EventApsConfirmStatus(event) != deCONZ::ApsSuccessStatus)
        {
            d->setState(DEV_InitStateHandler);
        }
    }
    else if (event.what() == REventActiveEndpoints)
    {
        d->stopStateTimer(StateLevel0);
        d->setState(DEV_InitStateHandler);
        DEV_EnqueueEvent(device, REventAwake);
    }
    else if (event.what() == REventStateTimeout)
    {
        DBG_Printf(DBG_INFO, "read ZDP active endpoints timeout: 0x%016llX\n", device->key());
        d->setState(DEV_InitStateHandler);
    }
}

/*! #4 This state checks that for all active endpoints simple descriptors are known.
 */
void DEV_SimpleDescriptorStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        quint8 needFetchEp = 0x00;

        for (const auto ep : device->node()->endpoints())
        {
            deCONZ::SimpleDescriptor sd;
            if (device->node()->copySimpleDescriptor(ep, &sd) != 0 || sd.deviceId() == 0xffff)
            {
                needFetchEp = ep;
                break;
            }
        }

        if (needFetchEp == 0x00)
        {
            DBG_Printf(DBG_INFO, "ZDP simple descriptors verified: 0x%016llX\n", device->key());
            d->setState(DEV_BasicClusterStateHandler);
        }
        else if (!device->reachable())
        {
            d->setState(DEV_InitStateHandler);
        }
        else
        {
            d->zdpResult = ZDP_SimpleDescriptorReq(device->item(RAttrNwkAddress)->toNumber(), needFetchEp, d->apsCtrl);
            if (d->zdpResult.isEnqueued)
            {
                d->startStateTimer(MinMacPollRxOn, StateLevel0);
            }
            else
            {
                d->setState(DEV_InitStateHandler);
            }
        }
    }
    else if (event.what() == REventApsConfirm)
    {
        Q_ASSERT(event.deviceKey() == device->key());
        if (d->zdpResult.apsReqId == EventApsConfirmId(event) && EventApsConfirmStatus(event) != deCONZ::ApsSuccessStatus)
        {
            d->setState(DEV_InitStateHandler);
        }
    }
    else if (event.what() == REventSimpleDescriptor)
    {
        d->stopStateTimer(StateLevel0);
        d->setState(DEV_InitStateHandler);
        DEV_EnqueueEvent(device, REventAwake);
    }
    else if (event.what() == REventStateTimeout)
    {
        DBG_Printf(DBG_INFO, "read ZDP simple descriptor timeout: 0x%016llX\n", device->key());
        d->setState(DEV_InitStateHandler);
    }
}

/*! Returns the first Simple Descriptor for a given server \p clusterId or nullptr if not found.
 */
const deCONZ::SimpleDescriptor *DEV_GetSimpleDescriptorForServerCluster(const Device *device, deCONZ::ZclClusterId_t clusterId)
{
    for (const auto &sd : device->node()->simpleDescriptors())
    {
        const auto cluster = std::find_if(sd.inClusters().cbegin(), sd.inClusters().cend(), [clusterId](const deCONZ::ZclCluster &cl)
        {
            return cl.id_t() == clusterId;
        });

        if (cluster != sd.inClusters().cend())
        {
            return &sd;
        }
    }

    return nullptr;
}

/*! Try to fill \c ResourceItem value from \p subDevices if not already set.
 */
bool DEV_FillItemFromSubdevices(Device *device, const char *itemSuffix, const std::vector<Resource*> &subDevices)
{
    auto *ditem = device->item(itemSuffix);
    Q_ASSERT(ditem);

    if (ditem->lastSet().isValid())
    {
        return true;
    }

    for (const auto rsub : subDevices)
    {
        auto *sitem = rsub->item(itemSuffix);
        if (sitem && sitem->lastSet().isValid())
        {
            // copy from sub-device into device
            if (ditem->setValue(sitem->toVariant()))
            {
                return true;
            }
        }
    }

    return false;
}

/*! Sends a ZCL Read Attributes request for \p clusterId and \p attrId.
    This also configures generic read and parse handlers for an \p item if not already set.
 */
bool DEV_ZclRead(Device *device, ResourceItem *item, deCONZ::ZclClusterId_t clusterId, deCONZ::ZclAttributeId_t attrId)
{
    Q_ASSERT(device);
    Q_ASSERT(item);

    DevicePrivate *d = device->d;

    if (!device->reachable())
    {
        DBG_Printf(DBG_INFO, "DEV not reachable, skip read %s: 0x%016llX\n", item->descriptor().suffix, device->key());
        return false;
    }

    const auto *sd = DEV_GetSimpleDescriptorForServerCluster(device, clusterId);

    if (!sd)
    {
        DBG_Printf(DBG_INFO, "TODO cluster 0x%04X not found: 0x%016llX\n", device->key(), static_cast<quint16>(clusterId));
        return false;
    }

    ZCL_Param param;
    param.valid = true;
    param.endpoint = sd->endpoint();
    param.clusterId = static_cast<quint16>(clusterId);
    param.attributes.push_back(static_cast<quint16>(attrId));

    const auto zclResult = ZCL_ReadAttributes(param, device->item(RAttrExtAddress)->toNumber(), device->item(RAttrNwkAddress)->toNumber(), d->apsCtrl);

    d->readResult.isEnqueued = zclResult.isEnqueued;
    d->readResult.apsReqId = zclResult.apsReqId;
    d->readResult.sequenceNumber = zclResult.sequenceNumber;

    return d->readResult.isEnqueued;
}

/*! #5 This state reads all common basic cluster attributes needed to match a DDF,
    e.g. modelId, manufacturer name, application version, etc.
 */
void DEV_BasicClusterStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        struct _item {
            const char *suffix;
            deCONZ::ZclClusterId_t clusterId;
            deCONZ::ZclAttributeId_t attrId;
        };

        const std::array<_item, 2> items = {
            _item{ RAttrManufacturerName, 0x0000_clid, 0x0004_atid },
            _item{ RAttrModelId,          0x0000_clid, 0x0005_atid }
        };

        size_t okCount = 0;
        const auto subDevices = device->subDevices();

        for (const auto &it : items)
        {
            if (DEV_FillItemFromSubdevices(device, it.suffix, subDevices))
            {
                okCount++;
                continue;
            }

            if (DEV_ZclRead(device, device->item(it.suffix), it.clusterId, it.attrId))
            {
                d->startStateTimer(MinMacPollRxOn, StateLevel0);
                return; // keep state and wait for REventStateTimeout or response
            }

            DBG_Printf(DBG_INFO, "Failed to read %s: 0x%016llX\n", it.suffix, device->key());
            break;
        }

        if (okCount != items.size())
        {
            d->setState(DEV_InitStateHandler);
        }
        else
        {
            DBG_Printf(DBG_INFO, "DEV modelId: %s, 0x%016llX\n", qPrintable(device->item(RAttrModelId)->toString()), device->key());
            d->setState(DEV_GetDeviceDescriptionHandler);
        }
    }
    else if (event.what() == REventApsConfirm)
    {
        Q_ASSERT(event.deviceKey() == device->key());
        if (d->readResult.apsReqId == EventApsConfirmId(event) && EventApsConfirmStatus(event) != deCONZ::ApsSuccessStatus)
        {
            d->setState(DEV_InitStateHandler);
        }
    }
    else if (event.what() == RAttrManufacturerName || event.what() == RAttrModelId)
    {
        DBG_Printf(DBG_INFO, "DEV received %s: 0x%016llX\n", event.what(), device->key());
        d->stopStateTimer(StateLevel0);
        d->setState(DEV_InitStateHandler); // ok re-evaluate
        DEV_EnqueueEvent(device, REventAwake);
    }
    else if (event.what() == REventStateTimeout)
    {
        DBG_Printf(DBG_INFO, "DEV read basic cluster timeout: 0x%016llX\n", device->key());
        d->setState(DEV_InitStateHandler);
    }
}

/*! #6 This state checks if for the device a device description file (DDF) is available.

    In that case the device is initialised (or updated) based on the JSON description.
    The actual processing is delegated to \c DeviceDescriptions class. This is done async
    so thousands of DDF files can be lazy loaded.
 */
void DEV_GetDeviceDescriptionHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        DEV_EnqueueEvent(device, REventDDFInitRequest);
    }
    else if (event.what() == REventDDFInitResponse)
    {
        if (event.num() == 1)
        {
            d->setState(DEV_IdleStateHandler);
        }
        else
        {
            d->setState(DEV_DeadStateHandler);
        }
    }
}

/*! #7 In this state the device is operational and runs sub states
    In parallel.

    IdleState : Bindings | Polling | ItemChange
 */
void DEV_IdleStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        d->setState(DEV_BindingHandler, STATE_LEVEL_BINDING);
        d->setState(DEV_PollIdleStateHandler, STATE_LEVEL_POLL);
        return;
    }
    else if (event.what() == REventStateLeave)
    {
        d->setState(nullptr, STATE_LEVEL_BINDING);
        d->setState(nullptr, STATE_LEVEL_POLL);
        return;
    }
    else if (event.what() == REventDDFReload)
    {
        d->setState(DEV_InitStateHandler);
    }
    else if (event.what() != RAttrLastSeen && event.what() != REventPoll)
    {
        // DBG_Printf(DBG_INFO, "DEV Idle event %s/0x%016llX/%s\n", event.resource(), event.deviceKey(), event.what());
    }

    DEV_CheckItemChanges(device, event);

    // process parallel states
    for (int i = StateLevel1; i < StateLevelMax; i++)
    {
        device->handleEvent(event, DEV_StateLevel(i));
    }
}

/*! Bindings sub state machien is described in:

      https://github.com/dresden-elektronik/deconz-rest-plugin-v2/wiki/Device-Class#bindings-sub-state-machine
*/

void DEV_BindingHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        DBG_Printf(DBG_INFO, "DEV Binding enter %s/0x%016llX\n", event.resource(), event.deviceKey());
    }

    if (event.what() == REventPoll || event.what() == REventAwake)
    {
        if (!d->binding.bindingVerify.isValid() || d->binding.bindingVerify.elapsed() > (1000 * 60 * 5))
        {
            DBG_Printf(DBG_INFO, "DEV Binding verify bindings %s/0x%016llX\n", event.resource(), event.deviceKey());
        }
        else
        {
            return;
        }
    }
    else if (event.what() == REventBindingTable)
    {
        if (event.num() == deCONZ::ZdpSuccess)
        {
            d->binding.mgmtBindSupported = true;
        }
        else if (event.num() == deCONZ::ZdpNotSupported)
        {
            d->binding.mgmtBindSupported = false;
        }
    }
    else
    {
        return;
    }

    d->binding.bindingIter = 0;
    d->setState(DEV_BindingTableVerifyHandler, STATE_LEVEL_BINDING);
    DEV_EnqueueEvent(device, REventBindingTick);
}

void DEV_BindingTableVerifyHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() != REventBindingTick)
    {

    }
    else if (d->binding.bindingIter >= device->node()->bindingTable().size())
    {
        d->binding.bindingVerify.start();
        d->setState(DEV_BindingHandler, STATE_LEVEL_BINDING);
    }
    else
    {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto &bnd = *(device->node()->bindingTable().const_begin() + d->binding.bindingIter);
        const auto dt = bnd.confirmedMsSinceEpoch() > 0 ? (now - bnd.confirmedMsSinceEpoch()) / 1000: -1;

        if (bnd.dstAddressMode() == deCONZ::ApsExtAddress)
        {
            DBG_Printf(DBG_INFO, "BND 0x%016llX cl: 0x%04X, dstAddrmode: %u, dst: 0x%016llX, dstEp: 0x%02X, dt: %lld seconds\n", bnd.srcAddress(), bnd.clusterId(), bnd.dstAddressMode(), bnd.dstAddress().ext(), bnd.dstEndpoint(), dt);
        }
        else if (bnd.dstAddressMode() == deCONZ::ApsGroupAddress)
        {
            DBG_Printf(DBG_INFO, "BND 0x%016llX cl: 0x%04X, dstAddrmode: %u, group: 0x%04X, dstEp: 0x%02X, dt: %lld seconds\n", bnd.srcAddress(), bnd.clusterId(), bnd.dstAddressMode(), bnd.dstAddress().group(), bnd.dstEndpoint(), dt);
        }

        d->binding.bindingIter++;
        DEV_EnqueueEvent(device, REventBindingTick);
    }
}

/*! Returns all items wich are ready for polling.
    The returned vector is reversed to use std::vector::pop_back() when processing the queue.
 */
std::vector<PollItem> DEV_GetPollItems(Device *device)
{
    std::vector<PollItem> result;
    const auto now = QDateTime::currentDateTime();

    for (const auto *r : device->subDevices())
    {
        for (int i = 0; i < r->itemCount(); i++)
        {
            const auto *item = r->itemForIndex(size_t(i));

            if (item->lastSet().isValid() && item->lastSet().secsTo(now) < item->refreshInterval())
            {
                continue;
            }

            const auto &ddfItem = DDF_GetItem(item);

            if (ddfItem.readParameters.isNull())
            {
                continue;
            }

            if (ddfItem.readParameters.toMap().empty())
            {
                continue;
            }

            result.emplace_back(PollItem{r, item, ddfItem.readParameters});
        }
    }

    std::reverse(result.begin(), result.end());
    return result;
}

/*! This state waits for REventPoll (and later REventPollForce).
    It collects all poll worthy items in a queue and moves to the PollNext state.
 */
void DEV_PollIdleStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        DBG_Printf(DBG_INFO, "DEV Poll Idle enter %s/0x%016llX\n", event.resource(), event.deviceKey());
    }
    else if (event.what() == REventPoll)
    {
        d->pollItems = DEV_GetPollItems(device);

        if (!d->pollItems.empty())
        {
            d->setState(DEV_PollNextStateHandler, STATE_LEVEL_POLL);
            return;
        }
    }
}

/*! This state processes the next PollItem and moves to the PollBusy state.
    If no more items are in the queue it moves back to PollIdle state.
 */
void DEV_PollNextStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter || event.what() == REventStateTimeout)
    {
        Q_ASSERT(event.num() == STATE_LEVEL_POLL); // TODO remove
        if (!device->reachable())
        {
            d->pollItems.clear();
        }

        if (d->pollItems.empty())
        {
            d->setState(DEV_PollIdleStateHandler, STATE_LEVEL_POLL);
            return;
        }

        auto &poll = d->pollItems.back();
        auto fn = DA_GetReadFunction(poll.readParameters);

        d->readResult = { };
        if (fn)
        {
            fn(poll.resource, poll.item, d->apsCtrl, poll.readParameters, &d->readResult);
        }
        else
        {
            DBG_Printf(DBG_INFO, "DEV: Poll Next no read function for item: %s / 0x%016llX\n", poll.item->descriptor().suffix, device->key());
            d->pollItems.pop_back();
        }

        if (d->readResult.isEnqueued)
        {
            d->setState(DEV_PollBusyStateHandler, STATE_LEVEL_POLL);
        }
        else
        {
            poll.retry++;

            DBG_Printf(DBG_INFO, "DEV: Poll Next failed to enqueue read item: %s / 0x%016llX\n", poll.item->descriptor().suffix, device->key());
            if (poll.retry >= MaxPollItemRetries)
            {
                d->pollItems.pop_back();
            }
            d->startStateTimer(MinMacPollRxOn, STATE_LEVEL_POLL); // try again
        }
    }
    else if (event.what() == REventStateLeave)
    {
        d->stopStateTimer(STATE_LEVEL_POLL);
    }
}

/*! This state waits for APS confirm or timeout for an ongoing poll request.
    In any case it moves back to PollNext state.
    If the request is successful the PollItem will be removed from the queue.
 */
void DEV_PollBusyStateHandler(Device *device, const Event &event)
{
    DevicePrivate *d = device->d;

    if (event.what() == REventStateEnter)
    {
        d->startStateTimer(MinMacPollRxOn, STATE_LEVEL_POLL);
    }
    else if (event.what() == REventStateLeave)
    {
        d->stopStateTimer(STATE_LEVEL_POLL);
    }
    else if (event.what() == REventApsConfirm && EventApsConfirmId(event) == d->readResult.apsReqId)
    {
        DBG_Printf(DBG_INFO, "DEV Poll Busy %s/0x%016llX APS confirm status: 0x%02X\n",
                   event.resource(), event.deviceKey(), EventApsConfirmStatus(event));
        Q_ASSERT(!d->pollItems.empty());

        if (EventApsConfirmStatus(event) == 0x00) // success
        {
            d->pollItems.pop_back();
        }
        else
        {
            auto &pollItem = d->pollItems.back();
            pollItem.retry++;

            if (pollItem.retry >= MaxPollItemRetries)
            {
                d->pollItems.pop_back();
            }
        }
        d->setState(DEV_PollNextStateHandler, STATE_LEVEL_POLL);
    }
    else if (event.what() == REventStateTimeout)
    {
        Q_ASSERT(event.num() == STATE_LEVEL_POLL); // TODO remove
        d->setState(DEV_PollNextStateHandler, STATE_LEVEL_POLL);
    }
}

/*! Empty handler to stop processing of the device.
 */
void DEV_DeadStateHandler(Device *device, const Event &event)
{
    Q_UNUSED(device)

    if (event.what() == REventStateEnter)
    {
        DBG_Printf(DBG_INFO, "DEV enter dead state 0x%016llX\n", event.deviceKey());
    }
}

Device::Device(DeviceKey key, deCONZ::ApsController *apsCtrl, QObject *parent) :
    QObject(parent),
    Resource(RDevices),
    d(new DevicePrivate)
{
    Q_ASSERT(parent);
    d->q = this;
    d->apsCtrl = apsCtrl;
    d->deviceKey = key;
    d->managed = DEV_TestManaged();

    addItem(DataTypeBool, RStateReachable);
    addItem(DataTypeBool, RAttrSleeper);
    addItem(DataTypeUInt64, RAttrExtAddress);
    addItem(DataTypeUInt16, RAttrNwkAddress);
    addItem(DataTypeString, RAttrUniqueId)->setValue(generateUniqueId(key, 0, 0));
    addItem(DataTypeString, RAttrManufacturerName);
    addItem(DataTypeString, RAttrModelId);

    d->setState(DEV_InitStateHandler);
}

Device::~Device()
{
    for (auto *r : subDevices())
    {
        r->setParentResource(nullptr);
    }

    Q_ASSERT(d);
    delete d;
    d = nullptr;
}

void Device::addSubDevice(Resource *sub)
{
    Q_ASSERT(sub);
    Q_ASSERT(sub->item(RAttrUniqueId));
    const auto uniqueId = sub->item(RAttrUniqueId)->toString();

    sub->setParentResource(this);

    for (const auto &s : d->subDevices)
    {
        if (std::get<0>(s) == uniqueId)
            return; // already registered
    }

    d->subDevices.push_back({uniqueId, sub->prefix()});
}

DeviceKey Device::key() const
{
    return d->deviceKey;
}

const deCONZ::Node *Device::node() const
{
    return d->node;
}

bool Device::managed() const
{
    return d->managed;
}

void Device::handleEvent(const Event &event, DEV_StateLevel level)
{
    if (event.what() == REventStateEnter || event.what() == REventStateLeave)
    {
        Q_ASSERT(event.num() >= StateLevel0);
        Q_ASSERT(event.num() < StateLevelMax);
        d->state[event.num()](this, event);
    }
    else if (d->state[level])
    {
        if (event.what() == REventAwake && level == StateLevel0)
        {
            d->awake.start();
        }

        d->state[level](this, event);
    }
}

void DevicePrivate::setState(DeviceStateHandler newState, DEV_StateLevel level)
{
    if (state[level] != newState)
    {
        if (state[level])
        {
            state[level](q, Event(q->prefix(), REventStateLeave, level, q->key()));
        }

        state[level] = newState;

        if (state[level])
        {
            emit q->eventNotify(Event(q->prefix(), REventStateEnter, level, q->key()));
        }
    }
}

void DevicePrivate::startStateTimer(int IntervalMs, DEV_StateLevel level)
{
    timer[level].start(IntervalMs, q);
}

void DevicePrivate::stopStateTimer(DEV_StateLevel level)
{
    if (timer[level].isActive())
    {
        timer[level].stop();
    }
}

void Device::timerEvent(QTimerEvent *event)
{
    for (int i = 0; i < StateLevelMax; i++)
    {
        if (event->timerId() == d->timer[i].timerId())
        {
            d->timer[i].stop(); // single shot
            d->state[i](this, Event(prefix(), REventStateTimeout, i, key()));
            break;
        }
    }
}

qint64 Device::lastAwakeMs() const
{
    return d->awake.isValid() ? d->awake.elapsed() : 8640000;
}

bool Device::reachable() const
{
    if (lastAwakeMs() < MinMacPollRxOn)
    {
        return true;
    }
    else if (node() && !node()->nodeDescriptor().isNull() && node()->nodeDescriptor().receiverOnWhenIdle())
    {
        return item(RStateReachable)->toBool();
    }
    else if (!item(RAttrSleeper)->toBool())
    {
        return item(RStateReachable)->toBool();
    }

    return false;
}

std::vector<Resource *> Device::subDevices() const
{
    std::vector<Resource *> result;

    // temp hack to get valid sub device pointers
    for (const auto &sub : d->subDevices)
    {
        auto *r = DEV_GetResource(std::get<1>(sub), std::get<0>(sub));

        if (r)
        {
            result.push_back(r);
        }
    }

    return result;
}

Device *DEV_GetDevice(DeviceContainer &devices, DeviceKey key)
{
    auto d = std::find_if(devices.begin(), devices.end(),
                          [key](const std::unique_ptr<Device> &device) { return device->key() == key; });

    if (d != devices.end())
    {
        return d->get();
    }

    return nullptr;
}

Device *DEV_GetOrCreateDevice(QObject *parent, deCONZ::ApsController *apsCtrl, DeviceContainer &devices, DeviceKey key)
{
    Q_ASSERT(key != 0);
    Q_ASSERT(apsCtrl);
    auto d = std::find_if(devices.begin(), devices.end(),
                          [key](const std::unique_ptr<Device> &device) { return device->key() == key; });

    if (d == devices.end())
    {
        devices.emplace_back(new Device(key, apsCtrl, parent));
        QObject::connect(devices.back().get(), SIGNAL(eventNotify(Event)), parent, SLOT(enqueueEvent(Event)));
        return devices.back().get();
    }

    Q_ASSERT(d != devices.end());

    return d->get();
}

bool DEV_RemoveDevice(DeviceContainer &devices, DeviceKey key)
{
    const auto i = std::find_if(devices.cbegin(), devices.cend(),
                          [key](const std::unique_ptr<Device> &device) { return device->key() == key; });
    if (i != devices.cend())
    {
        devices.erase(i);
    }

    return false;
}

/*! Is used to test full Device control over: Device and sub-device creation, read, write, parse of Zigbee commands.
 */
bool DEV_TestManaged()
{
    static bool managed = (deCONZ::appArgumentNumeric("--dev-test-managed", 0) > 0);
    return managed;
}
