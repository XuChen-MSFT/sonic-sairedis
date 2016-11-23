#include <iostream>
#include "syncd.h"
#include "sairedis.h"

std::mutex g_mutex;

swss::RedisClient           *g_redisClient = NULL;

std::map<std::string, std::string> gProfileMap;

bool g_veryFirstRun = false;

void exit_and_notify(int status)
{
    SWSS_LOG_ENTER();

    try
    {
        if (notifications != NULL)
        {
            std::vector<swss::FieldValueTuple> entry;

            SWSS_LOG_NOTICE("sending switch_shutdown_request notification to OA");

            notifications->send("switch_shutdown_request", "", entry);

            SWSS_LOG_NOTICE("notification send successfull");
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    catch(...)
    {
        SWSS_LOG_ERROR("Unknown runtime error");
    }

    exit(status);
}

#ifdef BRCMSAI
void sai_diag_shell()
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    while (true)
    {
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_SWITCH_SHELL_ENABLE;
        attr.value.booldata = true;
        status = sai_switch_api->set_switch_attribute(&attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to enable switch shell %d", status);
            return;
        }

        sleep(1);
    }
}
#endif /* BRCMSAI */

sai_object_id_t redis_create_virtual_object_id(
        _In_ sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    uint64_t virtual_id = g_redisClient->incr(VIDCOUNTER);

    sai_object_id_t vid = (((sai_object_id_t)object_type) << 48) | virtual_id;

    SWSS_LOG_DEBUG("created virtual object id %llx for object type %x", vid, object_type);

    return vid;
}

std::unordered_map<sai_object_id_t, sai_object_id_t> local_rid_to_vid;
std::unordered_map<sai_object_id_t, sai_object_id_t> local_vid_to_rid;

void save_rid_and_vid_to_local(
        _In_ sai_object_id_t rid,
        _In_ sai_object_id_t vid)
{
    SWSS_LOG_ENTER();

    local_rid_to_vid[rid] = vid;
    local_vid_to_rid[vid] = rid;
}

void remove_rid_and_vid_from_local(
        _In_ sai_object_id_t rid,
        _In_ sai_object_id_t vid)
{
    SWSS_LOG_ENTER();

    local_rid_to_vid.erase(rid);
    local_vid_to_rid.erase(vid);
}

sai_object_id_t translate_rid_to_vid(
        _In_ sai_object_id_t rid)
{
    SWSS_LOG_ENTER();

    if (rid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_DEBUG("translated RID null to VID null");

        return SAI_NULL_OBJECT_ID;
    }

    auto it = local_rid_to_vid.find(rid);

    if (it != local_rid_to_vid.end())
    {
        return it->second;
    }

    sai_object_id_t vid;

    std::string str_rid = sai_serialize_object_id(rid);
    std::string str_vid;

    auto pvid = g_redisClient->hget(RIDTOVID, str_rid);

    if (pvid != NULL)
    {
        // object exists
        str_vid = *pvid;

        sai_deserialize_object_id(str_vid, vid);

        SWSS_LOG_DEBUG("translated RID %llx to VID %llx", rid, vid);

        return vid;
    }

    SWSS_LOG_INFO("spotted new RID %llx", rid);

    sai_object_type_t object_type = sai_object_type_query(rid);

    if (object_type == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_ERROR("sai_object_type_query returned NULL type for RID %llx", rid);
        exit_and_notify(EXIT_FAILURE);
    }

    vid = redis_create_virtual_object_id(object_type);

    SWSS_LOG_DEBUG("translated RID %llx to VID %llx", rid, vid);

    str_vid = sai_serialize_object_id(vid);

    g_redisClient->hset(RIDTOVID, str_rid, str_vid);
    g_redisClient->hset(VIDTORID, str_vid, str_rid);

    save_rid_and_vid_to_local(rid, vid);

    return vid;
}

template <typename T>
void translate_list_rid_to_vid(
        _In_ T &element)
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < element.count; i++)
    {
        element.list[i] = translate_rid_to_vid(element.list[i]);
    }
}

void translate_rid_to_vid_list(
        _In_ sai_object_type_t object_type,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    // we receive real id's here, if they are new then create new id
    // for them and put in db, if entry exists in db, use it

    for (uint32_t i = 0; i < attr_count; i++)
    {
        sai_attribute_t &attr = attr_list[i];

        auto meta = get_attribute_metadata(object_type, attr.id);

        if (meta == NULL)
        {
            SWSS_LOG_ERROR("unable to get metadata for object type %x, attribute %x", object_type, attr.id);
            exit_and_notify(EXIT_FAILURE);
        }

        switch (meta->serializationtype)
        {
            case SAI_SERIALIZATION_TYPE_OBJECT_ID:
                attr.value.oid = translate_rid_to_vid(attr.value.oid);
                break;

            case SAI_SERIALIZATION_TYPE_OBJECT_LIST:
                translate_list_rid_to_vid(attr.value.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_FIELD_DATA_OBJECT_ID:
                attr.value.aclfield.data.oid = translate_rid_to_vid(attr.value.aclfield.data.oid);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_FIELD_DATA_OBJECT_LIST:
                translate_list_rid_to_vid(attr.value.aclfield.data.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_ACTION_DATA_OBJECT_ID:
                attr.value.aclaction.parameter.oid = translate_rid_to_vid(attr.value.aclaction.parameter.oid);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_ACTION_DATA_OBJECT_LIST:
                translate_list_rid_to_vid(attr.value.aclaction.parameter.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_PORT_BREAKOUT:
                translate_list_rid_to_vid(attr.value.portbreakout.port_list);

            default:
                break;
        }
    }
}

sai_object_id_t translate_vid_to_rid(
        _In_ sai_object_id_t vid)
{
    SWSS_LOG_ENTER();

    if (vid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_DEBUG("translated RID null to VID null");
        return SAI_NULL_OBJECT_ID;
    }

    auto it = local_vid_to_rid.find(vid);

    if (it != local_vid_to_rid.end())
    {
        return it->second;
    }

    std::string str_vid = sai_serialize_object_id(vid);

    std::string str_rid;

    auto prid = g_redisClient->hget(VIDTORID, str_vid);

    if (prid == NULL)
    {
        SWSS_LOG_ERROR("unable to get RID for VID: %s", str_vid.c_str());
        exit_and_notify(EXIT_FAILURE);
    }

    str_rid = *prid;

    sai_object_id_t rid;

    sai_deserialize_object_id(str_rid, rid);

    local_vid_to_rid[vid] = rid;

    SWSS_LOG_DEBUG("translated VID %llx to RID %llx", vid, rid);

    return rid;
}

void translate_vid_to_rid_list(
        _In_ sai_object_type_t object_type,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    // all id's reseived from sairedis should be virtual, so
    // lets translate them to real id's

    for (uint32_t i = 0; i < attr_count; i++)
    {
        sai_attribute_t &attr = attr_list[i];

        auto meta = get_attribute_metadata(object_type, attr.id);

        if (meta == NULL)
        {
            SWSS_LOG_ERROR("unable to get metadata for object type %x, attribute %x", object_type, attr.id);
            exit_and_notify(EXIT_FAILURE);
        }

        switch (meta->serializationtype)
        {
            case SAI_SERIALIZATION_TYPE_OBJECT_ID:
                attr.value.oid = translate_vid_to_rid(attr.value.oid);
                break;

            case SAI_SERIALIZATION_TYPE_OBJECT_LIST:
                translate_list_vid_to_rid(attr.value.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_FIELD_DATA_OBJECT_ID:
                attr.value.aclfield.data.oid = translate_vid_to_rid(attr.value.aclfield.data.oid);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_FIELD_DATA_OBJECT_LIST:
                translate_list_vid_to_rid(attr.value.aclfield.data.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_ACTION_DATA_OBJECT_ID:
                attr.value.aclaction.parameter.oid = translate_vid_to_rid(attr.value.aclaction.parameter.oid);
                break;

            case SAI_SERIALIZATION_TYPE_ACL_ACTION_DATA_OBJECT_LIST:
                translate_list_vid_to_rid(attr.value.aclaction.parameter.objlist);
                break;

            case SAI_SERIALIZATION_TYPE_PORT_BREAKOUT:
                translate_list_vid_to_rid(attr.value.portbreakout.port_list);

            default:
                break;
        }
    }
}

void internal_syncd_get_send(
        _In_ sai_object_type_t object_type,
        _In_ sai_status_t status,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    std::vector<swss::FieldValueTuple> entry;

    if (status == SAI_STATUS_SUCCESS)
    {
        translate_rid_to_vid_list(object_type, attr_count, attr_list);

        // XXX: normal serialization + translate reverse
        entry = SaiAttributeList::serialize_attr_list(
                object_type,
                attr_count,
                attr_list,
                false);
    }
    else if (status == SAI_STATUS_BUFFER_OVERFLOW)
    {
        // in this case we got correct values for list, but list was too small
        // so serialize only count without list itself, sairedis will need to take this
        // into accoung when deseralzie
        // if there was a list somewhere, count will be changed to actual value
        // different attributes can have different lists, many of them may
        // serialize only count, and will need to support that on the receiver

        entry = SaiAttributeList::serialize_attr_list(
                object_type,
                attr_count,
                attr_list,
                true);
    }
    else
    {
        // some other error, dont send attributes at all
    }

    std::string str_status = sai_serialize_status(status);

    std::string key = str_status;

    // since we have only one get at a time, we don't have to serialize
    // object type and object id, only get status is required
    // get response will not put any data to table only queue is used
    getResponse->set(key, entry, "getresponse");
}

swss::ProducerTable         *getResponse = NULL;
swss::NotificationProducer  *notifications = NULL;

const char* profile_get_value(
        _In_ sai_switch_profile_id_t profile_id,
        _In_ const char* variable)
{
    SWSS_LOG_ENTER();

    if (variable == NULL)
    {
        SWSS_LOG_WARN("variable is null");
        return NULL;
    }

    auto it = gProfileMap.find(variable);

    if (it == gProfileMap.end())
    {
        SWSS_LOG_INFO("%s: NULL", variable);
        return NULL;
    }

    SWSS_LOG_INFO("%s: %s", variable, it->second.c_str());

    return it->second.c_str();
}

std::map<std::string, std::string>::iterator gProfileIter = gProfileMap.begin();

int profile_get_next_value(
        _In_ sai_switch_profile_id_t profile_id,
        _Out_ const char** variable,
        _Out_ const char** value)
{
    SWSS_LOG_ENTER();

    if (value == NULL)
    {
        SWSS_LOG_INFO("resetting profile map iterator");

        gProfileIter = gProfileMap.begin();
        return 0;
    }

    if (variable == NULL)
    {
        SWSS_LOG_WARN("variable is null");
        return -1;
    }

    if (gProfileIter == gProfileMap.end())
    {
        SWSS_LOG_INFO("iterator reached end");
        return -1;
    }

    *variable = gProfileIter->first.c_str();
    *value = gProfileIter->second.c_str();

    SWSS_LOG_INFO("key: %s:%s", *variable, *value);

    gProfileIter++;

    return 0;
}

service_method_table_t test_services = {
    profile_get_value,
    profile_get_next_value
};

sai_status_t handle_generic(
        _In_ sai_object_type_t object_type,
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_object_id_t object_id;
    sai_deserialize_object_id(str_object_id, object_id);

    SWSS_LOG_DEBUG("common generic api: %d", api);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            {
                SWSS_LOG_DEBUG("generic create for object type %x", object_type);

                create_fn create = common_create[object_type];

                if (create == NULL)
                {
                    SWSS_LOG_ERROR("create function is not defined for object type %x", object_type);
                    exit_and_notify(EXIT_FAILURE);
                }

                sai_object_id_t real_object_id;
                sai_status_t status = create(&real_object_id, attr_count, attr_list);

                if (status == SAI_STATUS_SUCCESS)
                {
                    // object was created so new object id was generated
                    // we need to save virtual id's to redis db

                    std::string str_vid = sai_serialize_object_id(object_id);
                    std::string str_rid = sai_serialize_object_id(real_object_id);

                    g_redisClient->hset(VIDTORID, str_vid, str_rid);
                    g_redisClient->hset(RIDTOVID, str_rid, str_vid);

                    save_rid_and_vid_to_local(real_object_id, object_id);

                    SWSS_LOG_INFO("saved VID %s to RID %s", str_vid.c_str(), str_rid.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("failed to create %d", status);
                }

                return status;
            }

        case SAI_COMMON_API_REMOVE:
            {
                SWSS_LOG_DEBUG("generic remove for object type %x", object_type);

                remove_fn remove = common_remove[object_type];

                if (remove == NULL)
                {
                    SWSS_LOG_ERROR("remove function is not defined for object type %x", object_type);
                    exit_and_notify(EXIT_FAILURE);
                }

                sai_object_id_t rid = translate_vid_to_rid(object_id);

                std::string str_vid = sai_serialize_object_id(object_id);
                std::string str_rid = sai_serialize_object_id(rid);

                g_redisClient->hdel(VIDTORID, str_vid);
                g_redisClient->hdel(RIDTOVID, str_rid);

                remove_rid_and_vid_from_local(rid, object_id);

                return remove(rid);
            }

        case SAI_COMMON_API_SET:
            {
                SWSS_LOG_DEBUG("generic set for object type %x", object_type);

                set_attribute_fn set = common_set_attribute[object_type];

                if (set == NULL)
                {
                    SWSS_LOG_ERROR("set function is not defined for object type %x", object_type);
                    exit_and_notify(EXIT_FAILURE);
                }

                sai_object_id_t rid = translate_vid_to_rid(object_id);

                return set(rid, attr_list);
            }

        case SAI_COMMON_API_GET:
            {
                SWSS_LOG_DEBUG("generic get for object type %x", object_type);

                get_attribute_fn get = common_get_attribute[object_type];

                if (get == NULL)
                {
                    SWSS_LOG_ERROR("get function is not defined for object type %x", object_type);
                    exit_and_notify(EXIT_FAILURE);
                }

                sai_object_id_t rid = translate_vid_to_rid(object_id);

                return get(rid, attr_count, attr_list);
            }

        default:
            SWSS_LOG_ERROR("generic other apis not implemented");
            exit_and_notify(EXIT_FAILURE);
    }
}

sai_status_t handle_fdb(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_fdb_entry_t fdb_entry;
    sai_deserialize_fdb_entry(str_object_id, fdb_entry);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return sai_fdb_api->create_fdb_entry(&fdb_entry, attr_count, attr_list);

        case SAI_COMMON_API_REMOVE:
            return sai_fdb_api->remove_fdb_entry(&fdb_entry);

        case SAI_COMMON_API_SET:
            return sai_fdb_api->set_fdb_entry_attribute(&fdb_entry, attr_list);

        case SAI_COMMON_API_GET:
            return sai_fdb_api->get_fdb_entry_attribute(&fdb_entry, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("fdb other apis not implemented");
            exit_and_notify(EXIT_FAILURE);
    }
}

sai_status_t handle_switch(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return SAI_STATUS_NOT_SUPPORTED;

        case SAI_COMMON_API_REMOVE:
            return SAI_STATUS_NOT_SUPPORTED;

        case SAI_COMMON_API_SET:
            return sai_switch_api->set_switch_attribute(attr_list);

        case SAI_COMMON_API_GET:
            return sai_switch_api->get_switch_attribute(attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("switch other apis not implemented");
            exit_and_notify(EXIT_FAILURE);
    }
}

sai_status_t handle_neighbor(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_neighbor_entry_t neighbor_entry;
    sai_deserialize_neighbor_entry(str_object_id, neighbor_entry);

    neighbor_entry.rif_id = translate_vid_to_rid(neighbor_entry.rif_id);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return sai_neighbor_api->create_neighbor_entry(&neighbor_entry, attr_count, attr_list);

        case SAI_COMMON_API_REMOVE:
            return sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);

        case SAI_COMMON_API_SET:
            return sai_neighbor_api->set_neighbor_attribute(&neighbor_entry, attr_list);

        case SAI_COMMON_API_GET:
            return sai_neighbor_api->get_neighbor_attribute(&neighbor_entry, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("neighbor other apis not implemented");
            exit_and_notify(EXIT_FAILURE);
    }
}

sai_status_t handle_route(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_unicast_route_entry_t route_entry;
    sai_deserialize_route_entry(str_object_id, route_entry);

    route_entry.vr_id = translate_vid_to_rid(route_entry.vr_id);

    SWSS_LOG_DEBUG("route: %s", str_object_id.c_str());

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return sai_route_api->create_route(&route_entry, attr_count, attr_list);

        case SAI_COMMON_API_REMOVE:
            return sai_route_api->remove_route(&route_entry);

        case SAI_COMMON_API_SET:
            return sai_route_api->set_route_attribute(&route_entry, attr_list);

        case SAI_COMMON_API_GET:
            return sai_route_api->get_route_attribute(&route_entry, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("route other apis not implemented");
            exit_and_notify(EXIT_FAILURE);
    }
}

sai_status_t handle_vlan(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_vlan_id_t vlan_id;
    sai_deserialize_vlan_id(str_object_id, vlan_id);

    switch(api)
    {
        case SAI_COMMON_API_CREATE:
            return sai_vlan_api->create_vlan(vlan_id);

        case SAI_COMMON_API_REMOVE:
            return sai_vlan_api->remove_vlan(vlan_id);

        case SAI_COMMON_API_SET:
            return sai_vlan_api->set_vlan_attribute(vlan_id, attr_list);

        case SAI_COMMON_API_GET:
            return sai_vlan_api->get_vlan_attribute(vlan_id, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("vlan other apis not implemented");
            exit_and_notify(EXIT_FAILURE);
    }
}

sai_status_t handle_trap(
        _In_ std::string &str_object_id,
        _In_ sai_common_api_t api,
        _In_ uint32_t attr_count,
        _In_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    sai_hostif_trap_id_t trap_id;
    sai_deserialize_hostif_trap_id(str_object_id, trap_id);

    switch(api)
    {
        case SAI_COMMON_API_SET:
            return sai_hostif_api->set_trap_attribute(trap_id, attr_list);

        case SAI_COMMON_API_GET:
            return sai_hostif_api->get_trap_attribute(trap_id, attr_count, attr_list);

        default:
            SWSS_LOG_ERROR("trap other apis not implemented");
            exit_and_notify(EXIT_FAILURE);
    }
}

void sendResponse(sai_status_t status)
{
    SWSS_LOG_ENTER();

    std::string str_status = sai_serialize_status(status);

    std::vector<swss::FieldValueTuple> entry;

    SWSS_LOG_NOTICE("sending response: %s", str_status.c_str());

    getResponse->set(str_status, entry, "notify");
}

sai_status_t notifySyncd(const std::string& op)
{
    SWSS_LOG_ENTER();

    if (g_veryFirstRun)
    {
        SWSS_LOG_NOTICE("very first run is TRUE, op = %s", op.c_str());

        // on the very first start of syncd, "compile" view is directly
        // applied on device, since it will make it easier to switch
        // to new asic state later on when we restart orch agent

        if (op == SYNCD_APPLY_VIEW)
        {
            g_veryFirstRun = false;

            SWSS_LOG_NOTICE("setting very first run to FALSE, op = %s", op.c_str());
        }

        sendResponse(SAI_STATUS_SUCCESS);
        return SAI_STATUS_SUCCESS;
    }

    if (op == SYNCD_INIT_VIEW)
    {
        SWSS_LOG_WARN("op = %s - not implemented, but sending success", op.c_str());
        sendResponse(SAI_STATUS_SUCCESS);
    }
    else if (op == SYNCD_APPLY_VIEW)
    {
        SWSS_LOG_WARN("op = %s - not implemented, but sending success", op.c_str());
        sendResponse(SAI_STATUS_SUCCESS);
    }
    else
    {
        SWSS_LOG_ERROR("unknown operation: %s", op.c_str());
        sendResponse(SAI_STATUS_NOT_IMPLEMENTED);
        exit_and_notify(EXIT_FAILURE);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t processEvent(swss::ConsumerTable &consumer)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    SWSS_LOG_ENTER();

    swss::KeyOpFieldsValuesTuple kco;
    consumer.pop(kco);

    const std::string &key = kfvKey(kco);
    const std::string &op = kfvOp(kco);

    std::string str_object_type = key.substr(0, key.find(":"));
    std::string str_object_id = key.substr(key.find(":")+1);

    SWSS_LOG_INFO("key: %s op: %s", key.c_str(), op.c_str());

    sai_common_api_t api = SAI_COMMON_API_MAX;

    if (op == "create")
        api = SAI_COMMON_API_CREATE;
    else if (op == "remove")
        api = SAI_COMMON_API_REMOVE;
    else if (op == "set")
        api = SAI_COMMON_API_SET;
    else if (op == "get")
        api = SAI_COMMON_API_GET;
    else if (op == "notify")
        return notifySyncd(key);
    else
    {
        if (op != "delget")
            SWSS_LOG_ERROR("api %s is not implemented", op.c_str());

        return SAI_STATUS_NOT_SUPPORTED;
    }

    std::stringstream ss;

    sai_object_type_t object_type;
    sai_deserialize_object_type(str_object_type, object_type);

    if (object_type >= SAI_OBJECT_TYPE_MAX)
    {
        SWSS_LOG_ERROR("undefined object type %d", object_type);
        return SAI_STATUS_NOT_SUPPORTED;
    }

    const std::vector<swss::FieldValueTuple> &values = kfvFieldsValues(kco);

    SaiAttributeList list(object_type, values, false);

    if (api != SAI_COMMON_API_GET)
        translate_vid_to_rid_list(object_type, list.get_attr_count(), list.get_attr_list());

    sai_attribute_t *attr_list = list.get_attr_list();
    uint32_t attr_count = list.get_attr_count();

    sai_status_t status;
    switch (object_type)
    {
        case SAI_OBJECT_TYPE_FDB:
            status = handle_fdb(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_SWITCH:
            status = handle_switch(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_NEIGHBOR:
            status = handle_neighbor(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_ROUTE:
            status = handle_route(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_VLAN:
            status = handle_vlan(str_object_id, api, attr_count, attr_list);
            break;

        case SAI_OBJECT_TYPE_TRAP:
            status = handle_trap(str_object_id, api, attr_count, attr_list);
            break;

        default:
            status = handle_generic(object_type, str_object_id, api, attr_count, attr_list);
            break;
    }

    if (api == SAI_COMMON_API_GET)
    {
        internal_syncd_get_send(object_type, status, attr_count, attr_list);
    }
    else if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("failed to execute api: %s, key: %s, status: %s", op.c_str(), key.c_str(), sai_serialize_status(status).c_str());

        for (const auto&v: values)
        {
            SWSS_LOG_ERROR(" field: %s, value: %s", fvField(v).c_str(), fvValue(v).c_str());
        }

        exit_and_notify(EXIT_FAILURE);
    }

    return status;
}

swss::Logger::Priority redisGetLogLevel()
{
    SWSS_LOG_ENTER();

    auto plevel = g_redisClient->get("LOGLEVEL");

    if (plevel == NULL)
    {
        return swss::Logger::SWSS_NOTICE;
    }

    return swss::Logger::stringToPriority(*plevel);
}

void redisSetLogLevel(swss::Logger::Priority prio)
{
    SWSS_LOG_ENTER();

    std::string level = swss::Logger::priorityToString(prio);

    g_redisClient->set("LOGLEVEL", level);
}

void updateLogLevel()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto level = redisGetLogLevel();

    if (level != swss::Logger::getInstance().getMinPrio())
    {
        swss::Logger::getInstance().setMinPrio(level);

        SWSS_LOG_NOTICE("log level changed to %s", swss::Logger::priorityToString(level).c_str());

        // set level to correct one if user set some invalid value
        redisSetLogLevel(level);
    }
}

struct cmdOptions
{
    int countersThreadIntervalInSeconds;
    bool diagShell;
    bool useTempView;
    int startType;
    bool disableCountersThread;
    std::string profileMapFile;
#ifdef SAITHRIFT
    bool run_rpc_server;
    std::string portMapFile;
#endif // SAITHRIFT
    ~cmdOptions() {}
};

void printUsage()
{
    std::cout << "Usage: syncd [-N] [-d] [-p profile] [-i interval] [-t [cold|warm|fast]] [-h] [-u]" << std::endl;
    std::cout << "    -N --nocounters:" << std::endl;
    std::cout << "        Disable counter thread" << std::endl;
    std::cout << "    -d --diag:" << std::endl;
    std::cout << "        Enable diagnostic shell" << std::endl;
    std::cout << "    -p --profile profile:" << std::endl;
    std::cout << "        Provide profile map file" << std::endl;
    std::cout << "    -i --countersInterval interval:" << std::endl;
    std::cout << "        Provide counter thread interval" << std::endl;
    std::cout << "    -t --startType type:" << std::endl;
    std::cout << "        Specify cold|warm|fast start type" << std::endl;
    std::cout << "    -u --useTempView type:" << std::endl;
    std::cout << "        Use temporary view between init and apply" << std::endl;
#ifdef SAITHRIFT
    std::cout << "    -r --rpcserver:"           << std::endl;
    std::cout << "        Enable rpcserver"      << std::endl;
    std::cout << "    -m --portmap:"             << std::endl;
    std::cout << "        Specify port map file" << std::endl;
#endif // SAITHRIFT
    std::cout << "    -h --help:" << std::endl;
    std::cout << "        Print out this message" << std::endl;
}

cmdOptions handleCmdLine(int argc, char **argv)
{
    SWSS_LOG_ENTER();

    cmdOptions options;

    const int defaultCountersThreadIntervalInSeconds = 1;

    options.countersThreadIntervalInSeconds = defaultCountersThreadIntervalInSeconds;

#ifdef SAITHRIFT
    options.run_rpc_server = false;
    const char* const optstring = "dNt:p:i:rm:hu";
#else
    const char* const optstring = "dNt:p:i:hu";
#endif // SAITHRIFT

    while(true)
    {
        static struct option long_options[] =
        {
            { "useTempView",      no_argument,       0, 'u' },
            { "diag",             no_argument,       0, 'd' },
            { "nocounters",       no_argument,       0, 'N' },
            { "startType",        required_argument, 0, 't' },
            { "profile",          required_argument, 0, 'p' },
            { "countersInterval", required_argument, 0, 'i' },
            { "help",             no_argument,       0, 'h' },
#ifdef SAITHRIFT
            { "rpcserver",        no_argument,       0, 'r' },
            { "portmap",          required_argument, 0, 'm' },
#endif // SAITHRIFT
            { 0,                  0,                 0,  0  }
        };

        int option_index = 0;

        int c = getopt_long(argc, argv, optstring, long_options, &option_index);

        if (c == -1)
            break;

        switch (c)
        {
            case 'u':
                SWSS_LOG_NOTICE("enable use temp view");
                options.useTempView = true;
                break;

            case 'N':
                SWSS_LOG_NOTICE("disable counters thread");
                options.disableCountersThread = true;
                break;

            case 'd':
                SWSS_LOG_NOTICE("enable diag shell");
                options.diagShell = true;
                break;

            case 'p':
                SWSS_LOG_NOTICE("profile map file: %s", optarg);
                options.profileMapFile = std::string(optarg);
                break;

            case 'i':
                {
                    SWSS_LOG_NOTICE("counters thread interval: %s", optarg);

                    int interval = std::stoi(std::string(optarg));

                    if (interval == 0)
                    {
                        // use zero interval to disable counters thread
                        options.disableCountersThread = true;
                    }
                    else
                    {
                        options.countersThreadIntervalInSeconds =
                            std::max(defaultCountersThreadIntervalInSeconds, interval);
                    }

                    break;
                }

            case 't':
                SWSS_LOG_NOTICE("start type: %s", optarg);
                if (std::string(optarg) == "cold")
                {
                    options.startType = SAI_COLD_BOOT;
                }
                else if (std::string(optarg) == "warm")
                {
                    options.startType = SAI_WARM_BOOT;
                }
                else if (std::string(optarg) == "fast")
                {
                    options.startType = SAI_FAST_BOOT;
                }
                else
                {
                    SWSS_LOG_ERROR("unknown start type %s", optarg);
                    exit(EXIT_FAILURE);
                }
                break;

#ifdef SAITHRIFT
            case 'r':
                SWSS_LOG_NOTICE("enable rpc server");
                options.run_rpc_server = true;
                break;
            case 'm':
                SWSS_LOG_NOTICE("port map file: %s", optarg);
                options.portMapFile = std::string(optarg);
                break;
#endif // SAITHRIFT

            case 'h':
                printUsage();
                exit(EXIT_SUCCESS);

            case '?':
                SWSS_LOG_WARN("unknown option %c", optopt);
                printUsage();
                exit(EXIT_FAILURE);

            default:
                SWSS_LOG_ERROR("getopt_long failure");
                exit(EXIT_FAILURE);
        }
    }

    return options;
}

void handleProfileMap(const std::string& profileMapFile)
{
    SWSS_LOG_ENTER();

    if (profileMapFile.size() == 0)
        return;

    std::ifstream profile(profileMapFile);

    if (!profile.is_open())
    {
        SWSS_LOG_ERROR("failed to open profile map file: %s : %s", profileMapFile.c_str(), strerror(errno));
        exit(EXIT_FAILURE);
    }

    std::string line;

    while(getline(profile, line))
    {
        if (line.size() > 0 && (line[0] == '#' || line[0] == ';'))
            continue;

        size_t pos = line.find("=");

        if (pos == std::string::npos)
        {
            SWSS_LOG_WARN("not found '=' in line %s", line.c_str());
            continue;
        }

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        gProfileMap[key] = value;

        SWSS_LOG_INFO("insert: %s:%s", key.c_str(), value.c_str());
    }
}

#ifdef SAITHRIFT
std::map<std::set<int>, std::string> gPortMap;

// FIXME: introduce common config format for SONiC
void handlePortMap(const std::string& portMapFile)
{
    if (portMapFile.size() == 0)
        return;

    std::ifstream portmap(portMapFile);

    if (!portmap.is_open())
    {
        std::cerr << "failed to open port map file:" << portMapFile.c_str() << " : "<< strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    std::string line;

    while(getline(portmap, line))
    {
        if (line.size() > 0 && (line[0] == '#' || line[0] == ';'))
            continue;

        size_t pos = line.find(" ");

        if (pos == std::string::npos)
        {
            std::cerr << "port map parsing: not found ' ' in line" << line.c_str() << std::endl;
            continue;
        }

        std::string fp_value = line.substr(0, pos);
        std::string lanes    = line.substr(pos + 1);
        lanes.erase(lanes.begin(), std::find_if(lanes.begin(), lanes.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
        std::istringstream iss(lanes);
        std::string lane_str;
        std::set<int> lane_set;

        while (getline(iss, lane_str, ','))
        {
            int lane = stoi(lane_str);
            lane_set.insert(lane);
        }

        gPortMap.insert(std::pair<std::set<int>,std::string>(lane_set,fp_value));
    }
}
#endif // SAITHRIFT

bool handleRestartQuery(swss::NotificationConsumer &restartQuery)
{
    SWSS_LOG_ENTER();

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    restartQuery.pop(op, data, values);

    SWSS_LOG_DEBUG("op = %d", op.c_str());

    if (op == "COLD")
    {
        SWSS_LOG_NOTICE("received COLD switch shutdown event");
        return false;
    }

    if (op == "WARM")
    {
        SWSS_LOG_NOTICE("received WARM switch shutdown event");
        return true;
    }

    SWSS_LOG_WARN("received '%s' unknown switch shutdown event, assuming COLD", op.c_str());
    return false;
}

bool isVeryFirstRun()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    SWSS_LOG_ENTER();

    // if lane map is not defined in redis db then
    // we assume this is very first start of syncd
    // later on we can add additional checks here

    auto redisLaneMap = redisGetLaneMap();

    bool firstRun = redisLaneMap.size() == 0;

    SWSS_LOG_NOTICE("First Run: %s", firstRun ? "True" : "False");

    return firstRun;
}

int main(int argc, char **argv)
{
    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_DEBUG);

    SWSS_LOG_ENTER();

    swss::Logger::getInstance().setMinPrio(swss::Logger::SWSS_NOTICE);

    meta_init_db();

    auto options = handleCmdLine(argc, argv);

    handleProfileMap(options.profileMapFile);
#ifdef SAITHRIFT
    if (options.portMapFile.size() > 0)
    {
        handlePortMap(options.portMapFile);
    }
#endif // SAITHRIFT

    swss::DBConnector *db = new swss::DBConnector(ASIC_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);
    swss::DBConnector *dbNtf = new swss::DBConnector(ASIC_DB, swss::DBConnector::DEFAULT_UNIXSOCKET, 0);

    g_redisClient = new swss::RedisClient(db);

    updateLogLevel();

    swss::ConsumerTable *asicState = new swss::ConsumerTable(db, ASIC_STATE_TABLE);
    swss::NotificationConsumer *restartQuery = new swss::NotificationConsumer(db, "RESTARTQUERY");

    // at the end we cant use producer consumer concept since
    // if one proces will restart there may be something in the queue
    // also "remove" from response queue will also trigger another "response"
    getResponse  = new swss::ProducerTable(db, "GETRESPONSE");
    notifications = new swss::NotificationProducer(dbNtf, "NOTIFICATIONS");

    g_veryFirstRun = isVeryFirstRun();

    if (options.startType == SAI_WARM_BOOT)
    {
        const char *warmBootReadFile = profile_get_value(0, SAI_KEY_WARM_BOOT_READ_FILE);

        SWSS_LOG_NOTICE("using warmBootReadFile: '%s'", warmBootReadFile);

        if (warmBootReadFile == NULL || access(warmBootReadFile, F_OK) == -1)
        {
            SWSS_LOG_WARN("user requested warmStart but warmBootReadFile is not specified or not accesible, forcing cold start");

            options.startType = SAI_COLD_BOOT;
        }
    }

    if (options.startType == SAI_WARM_BOOT && g_veryFirstRun)
    {
        SWSS_LOG_WARN("warm start requested, but this is very first syncd start, forcing cold start");

        // we force cold start since if it's first run then redis db is not complete
        // so redis asic view will not reflect warm boot asic state, if this happen
        // then orch agent needs to be restarted as well to repopulate asic view
        options.startType = SAI_COLD_BOOT;
    }

    gProfileMap[SAI_KEY_BOOT_TYPE] = std::to_string(options.startType);

    sai_status_t status = sai_api_initialize(0, (service_method_table_t*)&test_services);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("fail to sai_api_initialize: %d", status);
        exit_and_notify(EXIT_FAILURE);
    }

    populate_sai_apis();

    initialize_common_api_pointers();

    status = sai_switch_api->initialize_switch(0, "", "", &switch_notifications);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("fail to sai_initialize_switch: %d", status);
        exit_and_notify(EXIT_FAILURE);
    }

#ifdef BRCMSAI

    if (options.diagShell)
    {
        SWSS_LOG_NOTICE("starting bcm diag shell thread");

        std::thread bcm_diag_shell_thread = std::thread(sai_diag_shell);
        bcm_diag_shell_thread.detach();
    }

#endif /* BRCMSAI */

#ifdef SAITHRIFT
    if (options.run_rpc_server)
    {
        start_sai_thrift_rpc_server(SWITCH_SAI_THRIFT_RPC_SERVER_PORT);
        SWSS_LOG_NOTICE("rpcserver started");
    }
#endif // SAITHRIFT

    SWSS_LOG_NOTICE("syncd started");

    bool warmRestartHint = false;

    try
    {
        onSyncdStart(options.startType == SAI_WARM_BOOT);

        if (options.disableCountersThread == false)
        {
            SWSS_LOG_NOTICE("starting counters thread");

            startCountersThread(options.countersThreadIntervalInSeconds);
        }

        SWSS_LOG_NOTICE("syncd listening for events");

        swss::Select s;

        s.addSelectable(asicState);
        s.addSelectable(restartQuery);

        while(true)
        {
            swss::Selectable *sel = NULL;

            int fd;

            int result = s.select(&sel, &fd);

            if (sel == restartQuery)
            {
                warmRestartHint = handleRestartQuery(*restartQuery);
                break;
            }

            if (result == swss::Select::OBJECT)
            {
                processEvent(*(swss::ConsumerTable*)sel);
            }
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());

        exit_and_notify(EXIT_FAILURE);
    }

    endCountersThread();

    if (warmRestartHint)
    {
        const char *warmBootWriteFile = profile_get_value(0, SAI_KEY_WARM_BOOT_WRITE_FILE);

        SWSS_LOG_NOTICE("using warmBootWriteFile: '%s'", warmBootWriteFile);

        if (warmBootWriteFile == NULL)
        {
            SWSS_LOG_WARN("user requested warm shutdown but warmBootWriteFile is not specified, forcing cold shutdown");

            warmRestartHint = false;
        }
    }

    sai_switch_api->shutdown_switch(warmRestartHint);

    SWSS_LOG_NOTICE("calling api uninitialize");

    sai_api_uninitialize();

    SWSS_LOG_NOTICE("uninitialize finished");

    return EXIT_SUCCESS;
}
