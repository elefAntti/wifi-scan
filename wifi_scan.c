/*
 * wifi-scan library implementation
 *
 * Copyright 2016-2018 (C) Bartosz Meglicki <meglickib@gmail.com>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

 /*
  * Library Overview
  *
  * This library uses netlink nl80211 user-space interface to retrieve wireless device information from kernel-space.
  * For netlink communication libmnl is used (minimalistic user-space netlink library).
  *
  * First concept you need to understand is that netlink uses sockets to communicate between user-space and kernel-space.
  *
  * There are 2 netlink communication channels (sockets/buffers)
  * - for notifications (about triggers, ready scan results)
  * - for commands (commanding triggers, retrieving scan results, station information)
  *
  * wifi_scan_init initializes 2 channels, gets nl80211 id using generic netlink (genetlink), gets id of
  * multicast group scan and subscribes to this group notifcations using notifications channel.
  *
  * wifi_scan_station gets the last (not necessarilly fresh) scan results that are available from the device,
  * checks which station we are associated with and retrieves information about this station (using commands channel)
  *
  * wifi_scan_all reads up any pending notifications, commands a trigger if necessary, waits for the device to gather
  * results and finally reads scan results with get_scan function (those are fresh results)
  *
  * wifi_scan_close frees up resources of two channels and any other resoureces that library uses.
  *
  * prepare_nl_messsage/send_nl_message/receive_nl_message are helper functions to simplify common tasks when issuing commands
  *
  * validate function simplifies common tasks (validates each attribute against table specifying what is valid)
  *
  */

#include "wifi_scan.h"

#include <libmnl/libmnl.h> //netlink libmnl
#include <linux/nl80211.h> //nl80211 netlink
#include <linux/genetlink.h> //generic netlink

#include <net/if.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <fcntl.h> //fntnl (set descriptor options)
#include <errno.h> //errno
#include <stdarg.h>

//Fix needed for compilation on Debian Wheezy
#ifndef NL80211_GENL_NAME
#define NL80211_GENL_NAME "nl80211"
#endif


// everything needed for sending/receiving with netlink
struct netlink_channel
{
  struct mnl_socket *nl; //netlink socket
  char *buf; //buffer for messages (in and out)
  uint16_t nl80211_id; //generic netlink nl80211 id
  uint32_t ifindex; //the wireless interface number (e.g. interface number for wlan0)
  uint32_t sequence; //the sequence number of netlink message
  void *context; //additional data to be stored/used when processing concrete message
};

// internal library data passed around by user
struct wifi_scan
{
  struct netlink_channel notification_channel;
  struct netlink_channel command_channel;
};

// DECLARATIONS AND TOP-DOWN LIBRARY OVERVIEW

// INITIALIZATION

// data needed from CTRL_CMD_GETFAMILY for nl80211, nl80211 id is stored in the channel rather then here
struct context_CTRL_CMD_GETFAMILY
{
  uint32_t id_NL80211_MULTICAST_GROUP_SCAN; //the id of group scan which we need to subscribe to
};

struct wifi_scan *wifi_scan_init(const char *interface);
// allocate memory, set initial values, etc.
static bool init_netlink_channel(struct netlink_channel *channel, const char *interface, char* buffer);
// create netlink sockets for generic netlink
static bool init_netlink_socket(struct netlink_channel *channel);

// execute command to get nl80211 family and process the results
static int get_family_and_scan_ids(struct netlink_channel *channel);
// this processes kernel reply for get family request, stores family id
static int handle_CTRL_CMD_GETFAMILY(const struct nlmsghdr *nlh, void *data);
// parses multicast groups to get scan multicast group id
static bool parse_CTRL_ATTR_MCAST_GROUPS(struct nlattr *nested, struct netlink_channel *channel);

// subscribes channel to multicast group scan using scan group id
static bool subscribe_NL80211_MULTICAST_GROUP_SCAN(struct netlink_channel *channel, uint32_t scan_group_id);

// CLEANUP

// public interface - cleans up after library
void wifi_scan_close(struct wifi_scan *wifi);
// cleans up after single channel
static void close_netlink_channel(struct netlink_channel *channel);

// SCANNING

// public interface - trigger scan if necessary, retrieve information about all known BSSes
int wifi_scan_all(struct wifi_scan *wifi, struct bss_info *bss_infos, int bss_infos_length);

// SCANNING - notification related

// the data needed from notifications
struct context_NL80211_MULTICAST_GROUP_SCAN
{
  int new_scan_results; //are new scan results waiting for us?
  int scan_triggered; //was scan was already triggered by somebody else?
};

// read but do not block
static bool read_past_notifications(struct netlink_channel *notifications);
// go non-blocking
static bool set_channel_non_blocking(struct netlink_channel *channel);
// go back blocking
static bool set_channel_blocking(struct netlink_channel *channel);
// this handles notifications
static int handle_NL80211_MULTICAST_GROUP_SCAN(const struct nlmsghdr *nlh, void *data);
// triggers scan if no results are waiting yet and if it was not already triggered
static int trigger_scan_if_necessary(struct netlink_channel *commands, struct context_NL80211_MULTICAST_GROUP_SCAN *scanning);
// triggers the scan
static int trigger_scan(struct netlink_channel *channel);
// wait for the notification that scan finished
static bool wait_for_new_scan_results(struct netlink_channel *notifications);


// LOGGING related stuff:
static void default_log_fcn(const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
  va_end(args);
}

wifi_scan_log_fcn log_fcn = &default_log_fcn;

#define to_log2(X, ...) log_fcn(X, __VA_ARGS__);
#define to_log(X) log_fcn("%s", X);

#define log_error(X) { \
  int error = errno;\
  to_log2("%s: %s", X, strerror(error));\
}

// SCANNING - scan related

// the data needed from new scan results
struct context_NL80211_CMD_NEW_SCAN_RESULTS
{
  struct bss_info *bss_infos;
  int bss_infos_length;
  int scanned;
};

// get scan results cached by the driver
static int get_scan(struct netlink_channel *channel);
// process the new scan results
static int handle_NL80211_CMD_NEW_SCAN_RESULTS(const struct nlmsghdr *nlh, void *data);
// get the information about bss (nested attribute)
static void parse_NL80211_ATTR_BSS(struct nlattr *nested, struct netlink_channel *channel);
// get the information from IE (non-netlink binary data here!)
static void parse_NL80211_BSS_INFORMATION_ELEMENTS(struct nlattr *attr, char SSID_OUT[33]);
// get BSSID (mac address)
static void parse_NL80211_BSS_BSSID(struct nlattr *attr, uint8_t bssid_out[BSSID_LENGTH]);

// STATION

// data needed from command new station
struct context_NL80211_CMD_NEW_STATION
{
  struct station_info *station;
};

// public interface - get information about station we are associated with
int wifi_scan_station(struct wifi_scan *wifi, struct station_info *station);
// get information about station with BSSID
static int get_station(struct netlink_channel *channel, uint8_t bssid[BSSID_LENGTH]);
// process command new station
static int handle_NL80211_CMD_NEW_STATION(const struct nlmsghdr *nlh, void *data);
// process station info (nested attribute)
static void parse_NL80211_ATTR_STA_INFO(struct nlattr *nested, struct netlink_channel *channel);

// NETLINK HELPERS

// NETLINK HELPERS - message construction/sending/receiving

// create the message with specified parameters for the channel
// fill the message with additional attributes as needed with:
// mnl_attr_put_[|u8|u16|u32|u64|str|strz] and mnl_attr_nest_[start|end]
static struct nlmsghdr *prepare_nl_message(uint32_t type, uint16_t flags, uint8_t genl_cmd, struct netlink_channel *channel);
// send the above message
static bool send_nl_message(struct nlmsghdr *nlh, struct netlink_channel *channel);
// receive the results and process them using callback function
static int receive_nl_message(struct netlink_channel *channel, mnl_cb_t callback);

// NETLINK HELPERS - validation

// formal requirements for attribute
struct attribute_validation
{
  int attr; // attribute constant from nl80211.h
  enum mnl_attr_data_type type; // MNL_TYPE_[UNSPEC|U8|U16|U32|U64|STRING|FLAG|MSECS|NESTED|NESTED_COMPAT|NUL_STRING|BINARY]
  size_t len;  // length in bytes, can be ommitted for attibutes of known size (e.g. U16), can be 0 if unspeciffied
};

// all information needed to validate attributes
struct validation_data
{
  struct nlattr **attribute_table; //validated attributes are returned here
  int attribute_length;  //at most that many, distinct constants from nl80211.h go here
  const struct attribute_validation *validation; //vavildate against that table
  int validation_length;
};

// data of type struct validation_data*, validate attr against data, this is called for each attribute
static int validate(const struct nlattr *attr, void *data);

// #####################################################################
// IMPLEMENTATION

// validate only what we are going to use, note that
// this lists all the attributes used by the library

const struct attribute_validation NL80211_VALIDATION[] = {
 {CTRL_ATTR_FAMILY_ID, MNL_TYPE_U16},
 {CTRL_ATTR_MCAST_GROUPS, MNL_TYPE_NESTED} };

const struct attribute_validation NL80211_MCAST_GROUPS_VALIDATION[] = {
 {CTRL_ATTR_MCAST_GRP_ID, MNL_TYPE_U32},
 {CTRL_ATTR_MCAST_GRP_NAME, MNL_TYPE_STRING} };

const struct attribute_validation NL80211_BSS_VALIDATION[] = {
 {NL80211_BSS_BSSID, MNL_TYPE_BINARY, 6},
 {NL80211_BSS_FREQUENCY, MNL_TYPE_U32},
 {NL80211_BSS_INFORMATION_ELEMENTS, MNL_TYPE_BINARY},
 {NL80211_BSS_STATUS, MNL_TYPE_U32},
 {NL80211_BSS_SIGNAL_MBM, MNL_TYPE_U32},
 {NL80211_BSS_SEEN_MS_AGO, MNL_TYPE_U32} };

const struct attribute_validation NL80211_NEW_SCAN_RESULTS_VALIDATION[] = {
 {NL80211_ATTR_IFINDEX, MNL_TYPE_U32},
 {NL80211_ATTR_SCAN_SSIDS, MNL_TYPE_NESTED},
 {NL80211_ATTR_BSS, MNL_TYPE_NESTED} };

const struct attribute_validation NL80211_CMD_NEW_STATION_VALIDATION[] = {
 {NL80211_ATTR_STA_INFO, MNL_TYPE_NESTED},
};

const struct attribute_validation NL80211_STA_INFO_VALIDATION[] = {
 {NL80211_STA_INFO_SIGNAL, MNL_TYPE_U8},
 {NL80211_STA_INFO_RX_PACKETS, MNL_TYPE_U32},
 {NL80211_STA_INFO_TX_PACKETS, MNL_TYPE_U32}
};

const int NL80211_VALIDATION_LENGTH = sizeof(NL80211_VALIDATION) / sizeof(struct attribute_validation);
const int NL80211_MCAST_GROUPS_VALIDATION_LENGTH = sizeof(NL80211_MCAST_GROUPS_VALIDATION) / sizeof(struct attribute_validation);
const int NL80211_BSS_VALIDATION_LENGTH = sizeof(NL80211_BSS_VALIDATION) / sizeof(struct attribute_validation);
const int NL80211_NEW_SCAN_RESULTS_VALIDATION_LENGTH = sizeof(NL80211_NEW_SCAN_RESULTS_VALIDATION) / sizeof(struct attribute_validation);
const int NL80211_CMD_NEW_STATION_VALIDATION_LENGTH = sizeof(NL80211_CMD_NEW_STATION_VALIDATION) / sizeof(struct attribute_validation);
const int NL80211_STA_INFO_VALIDATION_LENGTH = sizeof(NL80211_STA_INFO_VALIDATION) / sizeof(struct attribute_validation);


bool wifi_interface_exists(const char *interface)
{
  return if_nametoindex(interface) != 0;
}

// INITIALIZATION

static bool wifi_scan_init_internal(struct wifi_scan* wifi, char* buffer1, char* buffer2, const char *interface)
{

  if (!init_netlink_channel(&wifi->notification_channel, interface, buffer1))
  {
    return false;
  }

  struct context_CTRL_CMD_GETFAMILY family_context = { 0 };
  wifi->notification_channel.context = &family_context;

  if (get_family_and_scan_ids(&wifi->notification_channel) == -1)
  {
    to_log("GetFamilyAndScanId failed");
    return false;
  }

  if (family_context.id_NL80211_MULTICAST_GROUP_SCAN == 0)
  {
    to_log("No scan multicast group in generic netlink nl80211");
    return false;
  }

  if (!init_netlink_channel(&wifi->command_channel, interface, buffer2))
  {
    return false;
  }

  wifi->command_channel.nl80211_id = wifi->notification_channel.nl80211_id;

  if (!subscribe_NL80211_MULTICAST_GROUP_SCAN(&wifi->notification_channel, family_context.id_NL80211_MULTICAST_GROUP_SCAN))
  {
    return false;
  }

  return true;
}

// public interface - pass wireless interface like wlan0
struct wifi_scan* wifi_scan_init(const char *interface)
{
  struct wifi_scan* wifi = calloc(sizeof(struct wifi_scan), 1);
  char* buffer1 = (char*)malloc(MNL_SOCKET_BUFFER_SIZE);
  char* buffer2 = (char*)malloc(MNL_SOCKET_BUFFER_SIZE);

  if (wifi == NULL)
  {
    to_log("Can not allocate memory for wifi");
    return NULL;
  }

  if (!wifi_scan_init_internal(wifi, buffer1,  buffer2, interface))
  {
    wifi_scan_close(wifi);
    return NULL;
  }

  return wifi;
}

// prerequisities:
// - proper interface, e.g. wlan0, wlan1
static bool init_netlink_channel(struct netlink_channel *channel, const char *interface, char* buffer)
{
  channel->sequence = 1;
  channel->buf = buffer;
  channel->nl = 0;
  channel->ifindex = if_nametoindex(interface);

  if (channel->ifindex == 0)
  {
    to_log("Incorrect network interface");
    return false;
  }
  else
  {
    channel->context = NULL;
    return init_netlink_socket(channel);
  }
}

static bool init_netlink_socket(struct netlink_channel *channel)
{
  channel->nl = mnl_socket_open(NETLINK_GENERIC);

  if (channel->nl == NULL)
  {
    to_log("mnl_socket_open");
    return false;
  }

  if (mnl_socket_bind(channel->nl, 0, MNL_SOCKET_AUTOPID) < 0)
  {
    to_log("mnl_socket_bind");
    return false;
  }
  return true;
}

// prerequisities:
// - channel initialized with init_netlink_channel
// - channel context of type context_CTRL_CMD_GETFAMILY
static int get_family_and_scan_ids(struct netlink_channel *channel)
{
  struct nlmsghdr *nlh = prepare_nl_message(GENL_ID_CTRL, NLM_F_REQUEST | NLM_F_ACK, CTRL_CMD_GETFAMILY, channel);
  mnl_attr_put_u32(nlh, CTRL_ATTR_FAMILY_ID, GENL_ID_CTRL);
  mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME, NL80211_GENL_NAME);

  if (!send_nl_message(nlh, channel))
  {
    return MNL_CB_ERROR;
  }

  return receive_nl_message(channel, handle_CTRL_CMD_GETFAMILY);
}

// prerequisities:
// - netlink_channel passed as data
// - data->context of type struct context_CTRL_CMD_GETFAMILY
static int handle_CTRL_CMD_GETFAMILY(const struct nlmsghdr *nlh, void *data)
{
  struct nlattr *tb[CTRL_ATTR_MAX + 1] = {};
  struct genlmsghdr *genl = (struct genlmsghdr *)mnl_nlmsg_get_payload(nlh);
  struct netlink_channel *channel = (struct netlink_channel*)data;
  struct validation_data vd = { tb, CTRL_ATTR_MAX, NL80211_VALIDATION, NL80211_VALIDATION_LENGTH };

  mnl_attr_parse(nlh, sizeof(*genl), validate, &vd);

  if (!tb[CTRL_ATTR_FAMILY_ID])
  {
    to_log("No family id attribute");
    return MNL_CB_ERROR;
  }

  channel->nl80211_id = mnl_attr_get_u16(tb[CTRL_ATTR_FAMILY_ID]);

  if (tb[CTRL_ATTR_MCAST_GROUPS] 
      && !parse_CTRL_ATTR_MCAST_GROUPS(tb[CTRL_ATTR_MCAST_GROUPS], channel))
  {
    return MNL_CB_ERROR;
  }

  return MNL_CB_OK;
}

// prerequisities:
// - data->context of type struct context_CTRL_CMD_GETFAMILY
static bool parse_CTRL_ATTR_MCAST_GROUPS(struct nlattr *nested, struct netlink_channel *channel)
{
  struct nlattr *pos;

  mnl_attr_for_each_nested(pos, nested)
  {
    struct nlattr *tb[CTRL_ATTR_MCAST_GRP_MAX + 1] = {};
    struct validation_data vd = { tb, CTRL_ATTR_MCAST_GRP_MAX, NL80211_MCAST_GROUPS_VALIDATION, NL80211_MCAST_GROUPS_VALIDATION_LENGTH };

    mnl_attr_parse_nested(pos, validate, &vd);

    if (tb[CTRL_ATTR_MCAST_GRP_NAME])
    {
      const char *name = mnl_attr_get_str(tb[CTRL_ATTR_MCAST_GRP_NAME]);

      if (strcmp(name, "scan") == 0)
      {
        if (tb[CTRL_ATTR_MCAST_GRP_ID])
        {
          struct context_CTRL_CMD_GETFAMILY *context = channel->context;
          context->id_NL80211_MULTICAST_GROUP_SCAN = mnl_attr_get_u32(tb[CTRL_ATTR_MCAST_GRP_ID]);
        }
        else
        {
          to_log("Missing id attribute for scan multicast group");
          return false;
        }
      }
    }
  }
  return true;
}

// prerequisities:
// - channel initialized with init_netlink_channel
static bool subscribe_NL80211_MULTICAST_GROUP_SCAN(struct netlink_channel *channel, uint32_t scan_group_id)
{
  if (mnl_socket_setsockopt(channel->nl, NETLINK_ADD_MEMBERSHIP, &scan_group_id, sizeof(int)) < 0)
  {
    log_error("mnl_socket_set_sockopt");
    return false;
  }
  return true;
}

// CLEANUP

// prerequisities:
// - wifi initialized with wifi_scan_init
void wifi_scan_close(struct wifi_scan *wifi)
{
  close_netlink_channel(&wifi->notification_channel);
  close_netlink_channel(&wifi->command_channel);
}

// prerequisities:
// - channel initalized with init_netlink-channel
static void close_netlink_channel(struct netlink_channel *channel)
{
  free(channel->buf);
  if (channel->nl != 0)
  {
    mnl_socket_close(channel->nl);
  }
}


// SCANNING

// handle also trigger abort
// public interface
//
// prerequisities:
// - wifi initialized with wifi_scan_init
// - bss_info table of sized bss_info_length passed
int wifi_scan_all(struct wifi_scan *wifi, struct bss_info *bss_infos, int bss_infos_length)
{
  struct netlink_channel *notifications = &wifi->notification_channel;
  struct context_NL80211_MULTICAST_GROUP_SCAN scanning = { 0,0 };
  notifications->context = &scanning;

  struct netlink_channel *commands = &wifi->command_channel;
  struct context_NL80211_CMD_NEW_SCAN_RESULTS scan_results = { bss_infos, bss_infos_length, 0 };
  commands->context = &scan_results;

  //somebody else might have triggered scanning or even the results can be already waiting
  if (!read_past_notifications(notifications))
  {
    return -1;
  }

  //if no results yet or scan not triggered then trigger it.
  //the device can be busy - we have to take it into account
  if (trigger_scan_if_necessary(commands, &scanning) == -1)
    return -1; //most likely with errno set to EBUSY

//now just wait for trigger/new_scan_results
  if (!wait_for_new_scan_results(notifications))
  {
    return -1;
  }

  //finally read the scan
  get_scan(commands);

  return scan_results.scanned;
}

// SCANNING - notification related

// prerequisities
// - subscribed to scan group with subscribe_NL80211_MULTICAST_GROUP_SCAN
// - context_NL80211_MULTICAST_GROUP_SCAN set for notifications
static bool read_past_notifications(struct netlink_channel *notifications)
{
  if (!set_channel_non_blocking(notifications))
  {
    return false;
  }

  int ret, run_ret;

  while ((ret = mnl_socket_recvfrom(notifications->nl, notifications->buf, MNL_SOCKET_BUFFER_SIZE)) >= 0)
  {
    //the line below fills context about past scans/triggers
    run_ret = mnl_cb_run(notifications->buf, ret, 0, 0, handle_NL80211_MULTICAST_GROUP_SCAN, notifications);
    if (run_ret <= 0)
    {
      to_log("ReadPastNotificationsNonBlocking mnl_cb_run failed");
      return false;
    }
  }

  if (ret == -1)
  {
    if (!(errno == EINPROGRESS || errno == EWOULDBLOCK))
    {
      to_log("ReadPastNotificationsNonBlocking mnl_socket_recv failed");
      return false;
    }
  }
  //no more notifications waiting
  return set_channel_blocking(notifications);
}

// prerequisities
// - channel initialized with init_netlink_channel
static bool set_channel_non_blocking(struct netlink_channel *channel)
{
  int fd = mnl_socket_get_fd(channel->nl);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
  {
    log_error("SetChannelNonBlocking F_GETFL");
    return false;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
  {
    log_error("SetChannelNonBlocking F_SETFL");
    return false;
  }
  return true;
}

// prerequisities
// - channel initialized with init_netlink_channel
static bool set_channel_blocking(struct netlink_channel *channel)
{
  int fd = mnl_socket_get_fd(channel->nl);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
  {
    log_error("SetChannelNonBlocking F_GETFL");
    return false;
  }
  if (fcntl(fd, F_SETFL, flags &  ~O_NONBLOCK) == -1)
  {
    log_error("SetChannelNonBlocking F_SETFL");
    return false;
  }
  return true;
}

// prerequisities:
// - subscribed to scan group with subscribe_NL80211_MULTICAST_GROUP_SCAN
// - netlink_channel passed as data
// - data->context of type struct context_NL80211_MULTICAST_GROUP_SCAN
static int handle_NL80211_MULTICAST_GROUP_SCAN(const struct nlmsghdr *nlh, void *data)
{
  struct netlink_channel *channel = data;
  struct context_NL80211_MULTICAST_GROUP_SCAN *context = channel->context;

  struct genlmsghdr *genl = (struct genlmsghdr *)mnl_nlmsg_get_payload(nlh);

  if (genl->cmd == NL80211_CMD_TRIGGER_SCAN)
  {
    context->scan_triggered = 1;
    return MNL_CB_OK; //do nothing for now
  }
  else if (genl->cmd == NL80211_CMD_NEW_SCAN_RESULTS)
  {
    if (nlh->nlmsg_pid == 0 && nlh->nlmsg_seq == 0)
      context->new_scan_results = 1;
    return MNL_CB_OK; //do nothing for now
  }
  else
  {
    to_log2("Ignoring generic netlink command type %u seq %u pid  %u genl cmd %u\n", nlh->nlmsg_type, nlh->nlmsg_seq, nlh->nlmsg_pid, genl->cmd);
    return MNL_CB_OK;
  }
}


// prerequisities:
// - commands initialized with init_netlink_channel
// - scanning updated with read_past_notifications
static int trigger_scan_if_necessary(struct netlink_channel *commands, struct context_NL80211_MULTICAST_GROUP_SCAN *scanning)
{
  if (!scanning->new_scan_results && !scanning->scan_triggered)
    if (trigger_scan(commands) == -1)
      return -1; //most likely errno set to EBUSY which means hardware is doing something else, try again later
  return 0;
}

// prerequisities:
// - channel initialized with init_netlink_channel
static int trigger_scan(struct netlink_channel *channel)
{
  struct nlmsghdr *nlh = prepare_nl_message(channel->nl80211_id, NLM_F_REQUEST | NLM_F_ACK, NL80211_CMD_TRIGGER_SCAN, channel);
  mnl_attr_put_u32(nlh, NL80211_ATTR_IFINDEX, channel->ifindex);
  if (!send_nl_message(nlh, channel))
  {
    return MNL_CB_ERROR;
  }
  return receive_nl_message(channel, handle_NL80211_CMD_NEW_SCAN_RESULTS);
}

// prerequisities
// - channel initalized with init_netlink_channel
// - subscribed to scan group with subscribe_NL80211_MULTICAST_GROUP_SCAN
// - context_NL80211_MULTICAST_GROUP_SCAN set for notifications
static bool wait_for_new_scan_results(struct netlink_channel *notifications)
{
  struct context_NL80211_MULTICAST_GROUP_SCAN *scanning = notifications->context;
  int ret;

  while (!scanning->new_scan_results)
  {
    if ((ret = mnl_socket_recvfrom(notifications->nl, notifications->buf, MNL_SOCKET_BUFFER_SIZE)) <= 0)
    {
      to_log("Waiting for new scan results failed - mnl_socket_recvfrom");
      return false;
    }

    if ((ret = mnl_cb_run(notifications->buf, ret, 0, 0, handle_NL80211_MULTICAST_GROUP_SCAN, notifications)) <= 0)
    {
      to_log("Processing notificatoins failed - mnl_cb_run");
      return false;
    }
  }

  return true;
}

// SCANNING - scan related

// prerequisities:
// - channel initalized with init_netlink_channel
// - channel context of type context_NL80211_CMD_NEW_SCAN_RESULTS
static int get_scan(struct netlink_channel *channel)
{
  struct nlmsghdr *nlh = prepare_nl_message(channel->nl80211_id, NLM_F_REQUEST | NLM_F_DUMP | NLM_F_ACK, NL80211_CMD_GET_SCAN, channel);
  mnl_attr_put_u32(nlh, NL80211_ATTR_IFINDEX, channel->ifindex);

  if (!send_nl_message(nlh, channel))
  {
    return MNL_CB_ERROR;
  }
  return receive_nl_message(channel, handle_NL80211_CMD_NEW_SCAN_RESULTS);
}

// prerequisities:
// - netlink_channel passed as data
// - data->context of type context_NL80211_CMD_NEW_SCAN_RESULTS
static int handle_NL80211_CMD_NEW_SCAN_RESULTS(const struct nlmsghdr *nlh, void *data)
{
  struct netlink_channel *channel = data;
  struct nlattr *tb[NL80211_ATTR_MAX + 1] = {};
  struct validation_data vd = { tb, NL80211_ATTR_MAX, NL80211_NEW_SCAN_RESULTS_VALIDATION, NL80211_NEW_SCAN_RESULTS_VALIDATION_LENGTH };
  struct genlmsghdr *genl = (struct genlmsghdr *)mnl_nlmsg_get_payload(nlh);


  if (genl->cmd != NL80211_CMD_NEW_SCAN_RESULTS)
  {
    to_log2("Ignoring generic netlink command %u seq %u pid  %u genl cmd %u\n", nlh->nlmsg_type, nlh->nlmsg_seq, nlh->nlmsg_pid, genl->cmd);
    return MNL_CB_OK;
  }

  mnl_attr_parse(nlh, sizeof(*genl), validate, &vd);

  if (!tb[NL80211_ATTR_BSS])
    return MNL_CB_OK;

  parse_NL80211_ATTR_BSS(tb[NL80211_ATTR_BSS], channel);

  return MNL_CB_OK;
}

// prerequisities:
// - channel context of type context_NL80211_CMD_NEW_SCAN_RESULTS
static void parse_NL80211_ATTR_BSS(struct nlattr *nested, struct netlink_channel *channel)
{
  struct nlattr *tb[NL80211_BSS_MAX + 1] = {};
  struct validation_data vd = { tb, NL80211_BSS_MAX, NL80211_BSS_VALIDATION, NL80211_BSS_VALIDATION_LENGTH };
  struct context_NL80211_CMD_NEW_SCAN_RESULTS *scan_results = channel->context;
  struct bss_info *bss = scan_results->bss_infos + scan_results->scanned;

  mnl_attr_parse_nested(nested, validate, &vd);

  enum nl80211_bss_status status = BSS_NONE;

  if (tb[NL80211_BSS_STATUS])
    status = mnl_attr_get_u32(tb[NL80211_BSS_STATUS]);

  //if we have found associated station store first as last and associated as first
  if (status == NL80211_BSS_STATUS_ASSOCIATED
   //|| status == NL80211_BSS_STATUS_AUTHENTICATED
   || status == NL80211_BSS_STATUS_IBSS_JOINED)
  {
    if (scan_results->scanned > 0 && scan_results->scanned < scan_results->bss_infos_length)
      memcpy(bss, scan_results->bss_infos, sizeof(struct bss_info));
    bss = scan_results->bss_infos;
  }

  //check bounds, make exception if we have found associated station and replace previous data
  if (scan_results->bss_infos_length == 0 || (scan_results->scanned >= scan_results->bss_infos_length && bss != scan_results->bss_infos))
  {
    ++scan_results->scanned;
    return;
  }

  if (tb[NL80211_BSS_BSSID])
    parse_NL80211_BSS_BSSID(tb[NL80211_BSS_BSSID], bss->bssid);

  if (tb[NL80211_BSS_FREQUENCY])
    bss->frequency = mnl_attr_get_u32(tb[NL80211_BSS_FREQUENCY]);

  if (tb[NL80211_BSS_INFORMATION_ELEMENTS])
    parse_NL80211_BSS_INFORMATION_ELEMENTS(tb[NL80211_BSS_INFORMATION_ELEMENTS], bss->ssid);

  if (tb[NL80211_BSS_SIGNAL_MBM])
    bss->signal_mbm = mnl_attr_get_u32(tb[NL80211_BSS_SIGNAL_MBM]);

  if (tb[NL80211_BSS_SEEN_MS_AGO])
    bss->seen_ms_ago = mnl_attr_get_u32(tb[NL80211_BSS_SEEN_MS_AGO]);

  bss->status = (enum bss_status)status; //TODO: Better conversion

  ++scan_results->scanned;
}

//This is guesswork! Read up on that!!! I don't think it's netlink in this attribute, some lower beacon layer
static void parse_NL80211_BSS_INFORMATION_ELEMENTS(struct nlattr *attr, char SSID_OUT[SSID_MAX_LENGTH_WITH_NULL])
{
  const char *payload = mnl_attr_get_payload(attr);
  int len = mnl_attr_get_payload_len(attr);
  if (len == 0 || payload[0] != 0 || payload[1] >= SSID_MAX_LENGTH_WITH_NULL || payload[1] > len - 2)
  {
    to_log("SSID len 0 or payload not starting from 0 or payload length > 32 or payload length > length-2!");
    SSID_OUT[0] = '\0';
    return;
  }
  int ssid_len = payload[1];
  strncpy(SSID_OUT, payload + 2, ssid_len);
  SSID_OUT[ssid_len] = '\0';
}

static void parse_NL80211_BSS_BSSID(struct nlattr *attr, uint8_t bssid_out[BSSID_LENGTH])
{
  const char *payload = mnl_attr_get_payload(attr);
  int len = mnl_attr_get_payload_len(attr);

  if (len != BSSID_LENGTH)
  {
    to_log2("BSSID length != %d, ignoring", BSSID_LENGTH);
    memset(bssid_out, 0, BSSID_LENGTH);
    return;
  }

  memcpy(bssid_out, payload, BSSID_LENGTH);
}

// STATION

// public interface
//
// prerequisities:
// - wifi initialized with wifi_scan_init
int wifi_scan_station(struct wifi_scan *wifi, struct station_info *station)
{
  struct netlink_channel *commands = &wifi->command_channel;
  struct bss_info bss;

  struct context_NL80211_CMD_NEW_SCAN_RESULTS scan_results = { &bss, 1, 0 };
  commands->context = &scan_results;

  if (get_scan(commands) == MNL_CB_ERROR)
  {
    to_log("get_scan returned an error");
    return 0;
  }

  if (scan_results.scanned == 0)
    return 0;

  struct context_NL80211_CMD_NEW_STATION station_results = { station };
  commands->context = &station_results;

  if (get_station(commands, bss.bssid) == MNL_CB_ERROR)
  {
    to_log("get_station returned an error");
    return 0;
  }

  memcpy(station->bssid, bss.bssid, BSSID_LENGTH);
  memcpy(station->ssid, bss.ssid, SSID_MAX_LENGTH_WITH_NULL);
  station->status = bss.status;

  return 1;
}

// prerequisites:
// - channel initalized with init_netlink_channel
// - context_NL80211_CMD_NEW_STATION set for channel
static int get_station(struct netlink_channel *channel, uint8_t bssid[BSSID_LENGTH])
{
  struct nlmsghdr *nlh = prepare_nl_message(channel->nl80211_id, NLM_F_REQUEST | NLM_F_ACK, NL80211_CMD_GET_STATION, channel);
  mnl_attr_put_u32(nlh, NL80211_ATTR_IFINDEX, channel->ifindex);
  mnl_attr_put(nlh, NL80211_ATTR_MAC, BSSID_LENGTH, bssid);
  if (!send_nl_message(nlh, channel))
  {
    return MNL_CB_ERROR;
  }

  return receive_nl_message(channel, handle_NL80211_CMD_NEW_STATION);
}

// prerequisities:
// - netlink_channel passed as data
// - data->context of type context_NL80211_CMD_NEW_STATION
static int handle_NL80211_CMD_NEW_STATION(const struct nlmsghdr *nlh, void *data)
{
  struct netlink_channel *channel = data;
  struct nlattr *tb[NL80211_ATTR_MAX + 1] = {};
  struct validation_data vd = { tb, NL80211_ATTR_MAX, NL80211_CMD_NEW_STATION_VALIDATION, NL80211_CMD_NEW_STATION_VALIDATION_LENGTH };
  struct genlmsghdr *genl = (struct genlmsghdr *)mnl_nlmsg_get_payload(nlh);

  if (genl->cmd != NL80211_CMD_NEW_STATION)
  {
    to_log2("Ignoring generic netlink command %u seq %u pid  %u genl cmd %u\n", nlh->nlmsg_type, nlh->nlmsg_seq, nlh->nlmsg_pid, genl->cmd);
    return MNL_CB_OK;
  }

  mnl_attr_parse(nlh, sizeof(*genl), validate, &vd);

  if (!tb[NL80211_ATTR_STA_INFO]) //or error, no station
    return MNL_CB_OK;

  parse_NL80211_ATTR_STA_INFO(tb[NL80211_ATTR_STA_INFO], channel);

  return MNL_CB_OK;
}

// prerequisities:
// - channel context of type context_NL80211_CMD_NEW_STATION
static void parse_NL80211_ATTR_STA_INFO(struct nlattr *nested, struct netlink_channel *channel)
{
  struct nlattr *tb[NL80211_STA_INFO_MAX + 1] = {};
  struct validation_data vd = { tb, NL80211_STA_INFO_MAX, NL80211_STA_INFO_VALIDATION, NL80211_STA_INFO_VALIDATION_LENGTH };
  struct context_NL80211_CMD_NEW_STATION *station_results = channel->context;
  struct station_info *station = station_results->station;

  mnl_attr_parse_nested(nested, validate, &vd);

  if (tb[NL80211_STA_INFO_SIGNAL])
    station->signal_dbm = (int8_t)mnl_attr_get_u8(tb[NL80211_STA_INFO_SIGNAL]);
  if (tb[NL80211_STA_INFO_RX_PACKETS])
    station->rx_packets = mnl_attr_get_u32(tb[NL80211_STA_INFO_RX_PACKETS]);
  if (tb[NL80211_STA_INFO_TX_PACKETS])
    station->tx_packets = mnl_attr_get_u32(tb[NL80211_STA_INFO_TX_PACKETS]);
}


// NETLINK HELPERS

// NETLINK HELPERS - message construction/sending/receiving

// prerequisities:
// - channel initialized with init_netlink_channel
static struct nlmsghdr *prepare_nl_message(uint32_t type, uint16_t flags, uint8_t genl_cmd, struct netlink_channel *channel)
{
  struct nlmsghdr *nlh;
  struct genlmsghdr *genl;

  nlh = mnl_nlmsg_put_header(channel->buf);
  nlh->nlmsg_type = type;
  nlh->nlmsg_flags = flags;
  nlh->nlmsg_seq = channel->sequence;

  genl = (struct genlmsghdr*)mnl_nlmsg_put_extra_header(nlh, sizeof(struct genlmsghdr));
  genl->cmd = genl_cmd;
  genl->version = 1;
  return nlh;
}

// prerequisities:
// - prepare_nl_message called first
// - mnl_attr_put_xxx used if additional attributes needed
static bool send_nl_message(struct nlmsghdr *nlh, struct netlink_channel *channel)
{
  if (mnl_socket_sendto(channel->nl, nlh, nlh->nlmsg_len) < 0)
  {
    log_error("mnl_socket_sendto");
    return false;
  }
  return true;
}

// prerequisities:
// - send_nl_message called first
// - prerequisities for callback matched
static int receive_nl_message(struct netlink_channel *channel, mnl_cb_t callback)
{
  int ret;
  unsigned int portid = mnl_socket_get_portid(channel->nl);

  ret = mnl_socket_recvfrom(channel->nl, channel->buf, MNL_SOCKET_BUFFER_SIZE);

  while (ret > 0)
  {
    ret = mnl_cb_run(channel->buf, ret, channel->sequence, portid, callback, channel);
    if (ret <= 0)
      break;
    ret = mnl_socket_recvfrom(channel->nl, channel->buf, MNL_SOCKET_BUFFER_SIZE);
  }

  ++channel->sequence;

  return ret;
}

// NETLINK HELPERS - validation

// prerequisities:
// - data of type validation_data
static int validate(const struct nlattr *attr, void *data)
{
  struct validation_data *vd = data;
  const struct nlattr **tb = (const struct nlattr**) vd->attribute_table;
  int type = mnl_attr_get_type(attr), i;


  if (mnl_attr_type_valid(attr, vd->attribute_length) < 0)
    return MNL_CB_OK;

  for (i = 0; i < vd->validation_length; ++i)
    if (type == vd->validation[i].attr)
    {
      int len = vd->validation[i].len;
      if (len == 0 && mnl_attr_validate(attr, vd->validation[i].type) < 0)
      {
        log_error("mnl_attr_validate error");
        return MNL_CB_ERROR;
      }
      if (len != 0 && mnl_attr_validate2(attr, vd->validation[i].type, len) < 0)
      {
        log_error("mnl_attr_validate error");
        return MNL_CB_ERROR;
      }
    }

  tb[type] = attr;
  return MNL_CB_OK;
}

void wifi_scan_register_log_callback(wifi_scan_log_fcn fcn)
{
  log_fcn = fcn;
}