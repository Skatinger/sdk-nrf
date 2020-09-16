#include "coap_cloud.h"
#include <net/mqtt.h>
#include <net/socket.h>
#include <net/cloud.h>
#include <random/rand32.h>
#include <stdio.h>

#include <net/coap_utils.h>
#include <net/coap.h>
// #include <subsys/net/lib/coap_utils/>

#if defined(CONFIG_AWS_FOTA)
#include <net/aws_fota.h>
#endif

#include <logging/log.h>

LOG_MODULE_REGISTER(coap_cloud, CONFIG_COAP_CLOUD_LOG_LEVEL);
// LOG_MODULE_REGISTER(coap_cloud, 0);

//BUILD_ASSERT(sizeof(CONFIG_NRF_CLOUD_BROKER_HOST_NAME) > 1,
	//	 "AWS IoT hostname not set");

#if defined(CONFIG_COAP_CLOUD_IPV6)
#define AWS_AF_FAMILY AF_INET6
#else
#define AWS_AF_FAMILY AF_INET
#endif

#define AWS_TOPIC "things/"
#define AWS_TOPIC_LEN (sizeof(AWS_TOPIC) - 1)

#define AWS_CLIENT_ID_PREFIX "%s"
#define AWS_CLIENT_ID_LEN_MAX CONFIG_COAP_CLOUD_CLIENT_ID_MAX_LEN

// #define GET_TOPIC AWS_TOPIC "%s/shadow/get"
// #define GET_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 11)

// #define AWS_TOPIC "%s/shadow/update"
#define UPDATE_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 14)

// #define AWS_TOPIC "%s/shadow/delete"
// #define DELETE_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 14)

// static char client_id_buf[AWS_CLIENT_ID_LEN_MAX + 1];
// static char get_topic[GET_TOPIC_LEN + 1];
// static char update_topic[UPDATE_TOPIC_LEN + 1];
// static char delete_topic[DELETE_TOPIC_LEN + 1];

#if defined(CONFIG_COAP_CLOUD_TOPIC_UPDATE_ACCEPTED_SUBSCRIBE)
#define UPDATE_ACCEPTED_TOPIC AWS_TOPIC "%s/shadow/update/accepted"
#define UPDATE_ACCEPTED_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 23)
static char update_accepted_topic[UPDATE_ACCEPTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_COAP_CLOUD_TOPIC_UPDATE_REJECTED_SUBSCRIBE)
#define UPDATE_REJECTED_TOPIC AWS_TOPIC "%s/shadow/update/rejected"
#define UPDATE_REJECTED_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 23)
static char update_rejected_topic[UPDATE_REJECTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_COAP_CLOUD_TOPIC_UPDATE_DELTA_SUBSCRIBE)
#define UPDATE_DELTA_TOPIC AWS_TOPIC "%s/shadow/update/delta"
#define UPDATE_DELTA_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 20)
static char update_delta_topic[UPDATE_DELTA_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_COAP_CLOUD_TOPIC_GET_ACCEPTED_SUBSCRIBE)
#define GET_ACCEPTED_TOPIC AWS_TOPIC "%s/shadow/get/accepted"
#define GET_ACCEPTED_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 20)
static char get_accepted_topic[GET_ACCEPTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_COAP_CLOUD_TOPIC_GET_REJECTED_SUBSCRIBE)
#define GET_REJECTED_TOPIC AWS_TOPIC "%s/shadow/get/rejected"
#define GET_REJECTED_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 20)
static char get_rejected_topic[GET_REJECTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_COAP_CLOUD_TOPIC_DELETE_ACCEPTED_SUBSCRIBE)
#define DELETE_ACCEPTED_TOPIC AWS_TOPIC "%s/shadow/delete/accepted"
#define DELETE_ACCEPTED_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 23)
static char delete_accepted_topic[DELETE_ACCEPTED_TOPIC_LEN + 1];
#endif

#if defined(CONFIG_COAP_CLOUD_TOPIC_DELETE_REJECTED_SUBSCRIBE)
#define DELETE_REJECTED_TOPIC AWS_TOPIC "%s/shadow/delete/rejected"
#define DELETE_REJECTED_TOPIC_LEN (AWS_TOPIC_LEN + AWS_CLIENT_ID_LEN_MAX + 23)
static char delete_rejected_topic[DELETE_REJECTED_TOPIC_LEN + 1];
#endif

// static struct coap_cloud_app_topic_data app_topic_data;

// static char rx_buffer[CONFIG_COAP_CLOUD_COAP_RX_TX_BUFFER_LEN];
// static char tx_buffer[CONFIG_COAP_CLOUD_COAP_RX_TX_BUFFER_LEN];
// static char payload_buf[CONFIG_COAP_CLOUD_COAP_PAYLOAD_BUFFER_LEN];

static struct coap_client client;
// static struct sockaddr_storage broker;
static struct sockaddr_in broker; // TODO use ipv6 eventually
static int sock;

#if defined(CONFIG_CLOUD_API)
static struct cloud_backend *coap_cloud_backend;
#else
static coap_cloud_evt_handler_t module_evt_handler;
#endif

#define COAP_CLOUD_POLL_TIMEOUT_MS 500

static atomic_t disconnect_requested;
static atomic_t connection_poll_active;

static K_SEM_DEFINE(connection_poll_sem, 0, 1);

#if !defined(CONFIG_CLOUD_API)
static void coap_cloud_notify_event(const struct coap_cloud_evt *evt)
{
	printk("coap_cloud_notify_event with event");
	if ((module_evt_handler != NULL) && (evt != NULL)) {
		printk("even got to if\n");
		module_evt_handler(evt);
	}
}
#endif


#if defined(CONFIG_AWS_FOTA)
static void aws_fota_cb_handler(struct aws_fota_event *fota_evt)
{
#if defined(CONFIG_CLOUD_API)
	struct cloud_backend_config *config = coap_cloud_backend->config;
	struct cloud_event cloud_evt = { 0 };
#else
	struct coap_cloud_evt coap_cloud_evt = { 0 };
#endif

	if (fota_evt == NULL) {
		return;
	}

	switch (fota_evt->id) {
	case AWS_FOTA_EVT_START:
		LOG_DBG("AWS_FOTA_EVT_START");
#if defined(CONFIG_CLOUD_API)
		cloud_evt.type = CLOUD_EVT_FOTA_START;
		cloud_notify_event(coap_cloud_backend, &cloud_evt,
				   config->user_data);
#else
		coap_cloud_evt.type = COAP_CLOUD_EVT_FOTA_START;
		coap_cloud_notify_event(&coap_cloud_evt);
#endif
		break;
	case AWS_FOTA_EVT_DONE:
		LOG_DBG("AWS_FOTA_EVT_DONE");
#if defined(CONFIG_CLOUD_API)
		cloud_evt.type = CLOUD_EVT_FOTA_DONE;
		cloud_notify_event(coap_cloud_backend, &cloud_evt,
				   config->user_data);
#else
		coap_cloud_evt.type = COAP_CLOUD_EVT_FOTA_DONE;
		coap_cloud_notify_event(&coap_cloud_evt);
#endif
		break;
	case AWS_FOTA_EVT_ERASE_PENDING:
		LOG_DBG("AWS_FOTA_EVT_ERASE_PENDING");
#if defined(CONFIG_CLOUD_API)
		cloud_evt.type = CLOUD_EVT_FOTA_ERASE_PENDING;
		cloud_notify_event(coap_cloud_backend, &cloud_evt,
				   config->user_data);
#else
		coap_cloud_evt.type = COAP_CLOUD_EVT_FOTA_ERASE_PENDING;
		coap_cloud_notify_event(&coap_cloud_evt);
#endif
		break;
	case AWS_FOTA_EVT_ERASE_DONE:
		LOG_DBG("AWS_FOTA_EVT_ERASE_DONE");
#if defined(CONFIG_CLOUD_API)
		cloud_evt.type = CLOUD_EVT_FOTA_ERASE_DONE;
		cloud_notify_event(coap_cloud_backend, &cloud_evt,
				   config->user_data);
#else
		coap_cloud_evt.type = COAP_CLOUD_EVT_FOTA_ERASE_DONE;
		coap_cloud_notify_event(&coap_cloud_evt);
#endif
		break;
	case AWS_FOTA_EVT_ERROR:
		LOG_ERR("AWS_FOTA_EVT_ERROR");
		break;
	case AWS_FOTA_EVT_DL_PROGRESS:
		LOG_DBG("AWS_FOTA_EVT_DL_PROGRESS");
		break;
	default:
		LOG_ERR("Unknown FOTA event");
		break;
	}
}
#endif

// static int topic_subscribe(void)
// {
// 	int err;
// 	const struct mqtt_topic mqtt_cloud_rx_list[] = {
// #if defined(CONFIG_COAP_CLOUD_TOPIC_GET_ACCEPTED_SUBSCRIBE)
// 		{
// 			.topic = {
// 				.utf8 = get_accepted_topic,
// 				.size = strlen(get_accepted_topic)
// 			},
// 			.qos = COAP_QOS_1_AT_LEAST_ONCE
// 		},
// #endif
// #if defined(CONFIG_COAP_CLOUD_TOPIC_GET_REJECTED_SUBSCRIBE)
// 		{
// 			.topic = {
// 				.utf8 = get_rejected_topic,
// 				.size = strlen(get_rejected_topic)
// 			},
// 			.qos = COAP_QOS_1_AT_LEAST_ONCE
// 		},
// #endif
// #if defined(CONFIG_COAP_CLOUD_TOPIC_UPDATE_ACCEPTED_SUBSCRIBE)
// 		{
// 			.topic = {
// 				.utf8 = update_accepted_topic,
// 				.size = strlen(update_accepted_topic)
// 			},
// 			.qos = COAP_QOS_1_AT_LEAST_ONCE
// 		},
// #endif
// #if defined(CONFIG_COAP_CLOUD_TOPIC_UPDATE_REJECTED_SUBSCRIBE)
// 		{
// 			.topic = {
// 				.utf8 = update_rejected_topic,
// 				.size = strlen(update_rejected_topic)
// 			},
// 			.qos = COAP_QOS_1_AT_LEAST_ONCE
// 		},
// #endif
// #if defined(CONFIG_COAP_CLOUD_TOPIC_UPDATE_DELTA_SUBSCRIBE)
// 		{
// 			.topic = {
// 				.utf8 = update_delta_topic,
// 				.size = strlen(update_delta_topic)
// 			},
// 			.qos = COAP_QOS_1_AT_LEAST_ONCE
// 		},
// #endif
// #if defined(CONFIG_COAP_CLOUD_TOPIC_DELETE_ACCEPTED_SUBSCRIBE)
// 		{
// 			.topic = {
// 				.utf8 = delete_accepted_topic,
// 				.size = strlen(delete_accepted_topic)
// 			},
// 			.qos = COAP_QOS_1_AT_LEAST_ONCE
// 		},
// #endif
// #if defined(CONFIG_COAP_CLOUD_TOPIC_DELETE_REJECTED_SUBSCRIBE)
// 		{
// 			.topic = {
// 				.utf8 = delete_rejected_topic,
// 				.size = strlen(delete_rejected_topic)
// 			},
// 			.qos = COAP_QOS_1_AT_LEAST_ONCE
// 		},
// #endif
// 	};
//
// 	if (app_topic_data.list_count > 0) {
// 		const struct mqtt_subscription_list app_sub_list = {
// 			.list = app_topic_data.list,
// 			.list_count = app_topic_data.list_count,
// 			.message_id = sys_rand32_get()
// 		};
//
// 		for (size_t i = 0; i < app_sub_list.list_count; i++) {
// 			LOG_DBG("Subscribing to application topic: %s",
// 				log_strdup(app_sub_list.list[i].topic.utf8));
// 		}
//
// 		err = mqtt_subscribe(&client, &app_sub_list);
// 		if (err) {
// 			LOG_ERR("Application topics subscribe, error: %d", err);
// 		}
// 	}
//
// 	if (ARRAY_SIZE(mqtt_cloud_rx_list) > 0) {
// 		const struct mqtt_subscription_list aws_sub_list = {
// 			.list = (struct mqtt_topic *)&mqtt_cloud_rx_list,
// 			.list_count = ARRAY_SIZE(mqtt_cloud_rx_list),
// 			.message_id = sys_rand32_get()
// 		};
//
// 		for (size_t i = 0; i < aws_sub_list.list_count; i++) {
// 			LOG_DBG("Subscribing to AWS shadow topic: %s",
// 				log_strdup(aws_sub_list.list[i].topic.utf8));
// 		}
//
// 		err = mqtt_subscribe(&client, &aws_sub_list);
// 		if (err) {
// 			LOG_ERR("AWS shadow topics subscribe, error: %d", err);
// 		}
// 	}
//
// 	return err;
// }

// static int publish_get_payload(size_t length) // TODO add client here struct coap_client *const c, size_t length)
// {
// 	if (length > sizeof(payload_buf)) {
// 		LOG_ERR("Incoming COAP message too large for payload buffer");
// 		return -EMSGSIZE;
// 	}
//
// 	// return mqtt_readall_publish_payload(c, payload_buf, length);
// }

static void coap_evt_handler(struct coap_client *const c,
															const struct coap_evt *coap_evt) // TODO add client here struct coap_client *const c)
			     // const struct mqtt_evt *mqtt_evt)
{

	printk("coap_evt_handler got called ffffffffffffffffffffffffffffffffffff\n");
	int err;
#if defined(CONFIG_CLOUD_API)
	struct cloud_backend_config *config = coap_cloud_backend->config;
	struct cloud_event cloud_evt = { 0 };
#else
	struct coap_cloud_evt coap_cloud_evt = { 0 };
#endif

#if defined(CONFIG_AWS_FOTA)
	err = aws_fota_mqtt_evt_handler(c, coap_evt);
	if (err == 0) {
		/* Event handled by FOTA library so it can be skipped. */
		return;
	} else if (err < 0) {
		LOG_ERR("aws_fota_mqtt_evt_handler, error: %d", err);
		LOG_DBG("Disconnecting COAP client...");

		atomic_set(&disconnect_requested, 1);
		err = coap_disconnect(c);
		if (err) {
			LOG_ERR("Could not disconnect: %d", err);
		}
	}
#endif

  printf("in coap_evt_handler with type of event \n");
  // TODO switch over possible events (if any?)
// 	switch (coap_evt->type) {
// 	case COAP_EVT_CONNACK:
//
// 		if (coap_evt->param.connack.return_code) {
// 			LOG_ERR("COAP_EVT_CONNACK, error: %d",
// 				coap_evt->param.connack.return_code);
// #if defined(CONFIG_CLOUD_API)
// 			cloud_evt.data.err =
// 				coap_evt->param.connack.return_code;
// 			cloud_evt.type = CLOUD_EVT_ERROR;
// 			cloud_notify_event(coap_cloud_backend, &cloud_evt,
// 				   config->user_data);
// #else
// 			coap_cloud_evt.data.err =
// 				coap_evt->param.connack.return_code;
// 			coap_cloud_evt.type = COAP_CLOUD_EVT_ERROR;
// 			coap_cloud_notify_event(&coap_cloud_evt);
// #endif
// 			break;
// 		}


#if defined(CONFIG_CLOUD_API)
// 			cloud_evt.data.err =
// 				coap_evt->param.connack.return_code;
// 			cloud_evt.type = CLOUD_EVT_ERROR;
// 			cloud_notify_event(coap_cloud_backend, &cloud_evt,
// 				   config->user_data);
#else
// 			coap_cloud_evt.data.err =
// 				coap_evt->param.connack.return_code;
  coap_cloud_evt.type = COAP_CLOUD_EVT_CONNECTED;
	printk("notifying with coap_cloud_evt\n");
	coap_cloud_notify_event(&coap_cloud_evt);
#endif

		// if (!coap_evt->param.connack.session_present_flag) {
		// 	topic_subscribe();
		// }

		LOG_DBG("COAP client connected!");

#if defined(CONFIG_CLOUD_API)
		//cloud_evt.data.persistent_session =
			//	   coap_evt->param.connack.session_present_flag;
		cloud_evt.type = CLOUD_EVT_CONNECTED;
		cloud_notify_event(coap_cloud_backend, &cloud_evt,
				   config->user_data);
		cloud_evt.type = CLOUD_EVT_READY;
		cloud_notify_event(coap_cloud_backend, &cloud_evt,
				   config->user_data);
#else
		// coap_cloud_evt.data.persistent_session =
				   // coap_evt->param.connack.session_present_flag;
		coap_cloud_evt.type = COAP_CLOUD_EVT_CONNECTED;
		coap_cloud_notify_event(&coap_cloud_evt);
		coap_cloud_evt.type = COAP_CLOUD_EVT_READY;
		coap_cloud_notify_event(&coap_cloud_evt);
#endif
		//break;
	//case COAP_CLOUD_EVT_DISCONNECT:
		//LOG_DBG("COAP_EVT_DISCONNECT: result = %d", coap_evt->result);

#if defined(CONFIG_CLOUD_API)
		cloud_evt.type = CLOUD_EVT_DISCONNECTED;
		cloud_notify_event(coap_cloud_backend, &cloud_evt,
				   config->user_data);
#else
		coap_cloud_evt.type = COAP_CLOUD_EVT_DISCONNECTED;
		coap_cloud_notify_event(&coap_cloud_evt);
#endif
	// break;
	// case COAP_CLOUD_EVT_PUBLISH: {
	// 	const struct mqtt_publish_param *p = &coap_evt->param.publish;
	//
	// 	LOG_DBG("COAP_EVT_PUBLISH: id = %d len = %d ",
	// 		p->message_id,
	// 		p->message.payload.len);
	//
	// 	err = publish_get_payload(c, p->message.payload.len);
	// 	if (err) {
	// 		LOG_ERR("publish_get_payload, error: %d", err);
	// 		break;
	// 	}
	//
	// 	if (p->message.topic.qos == COAP_QOS_1_AT_LEAST_ONCE) {
	// 		const struct mqtt_puback_param ack = {
	// 			.message_id = p->message_id
	// 		};
	//
	// 		mqtt_publish_qos1_ack(c, &ack);
	// 	}

#if defined(CONFIG_CLOUD_API)
		cloud_evt.type = CLOUD_EVT_DATA_RECEIVED;
		// cloud_evt.data.msg.buf = payload_buf;
		// cloud_evt.data.msg.len = p->message.payload.len;
		// cloud_evt.data.msg.endpoint.type = CLOUD_EP_TOPIC_MSG;
		// cloud_evt.data.msg.endpoint.str = p->message.topic.topic.utf8;
		// cloud_evt.data.msg.endpoint.len = p->message.topic.topic.size;
		// TODO implement version for COAP

		cloud_notify_event(coap_cloud_backend, &cloud_evt,
				   config->user_data);
#else
		coap_cloud_evt.type = COAP_CLOUD_EVT_DATA_RECEIVED;
		printk("received data\n");
		// coap_cloud_evt.data.msg.ptr = payload_buf;
		// coap_cloud_evt.data.msg.len = p->message.payload.len;
		// coap_cloud_evt.data.msg.topic.type = COAP_CLOUD_SHADOW_TOPIC_UNKNOWN;
		// coap_cloud_evt.data.msg.topic.str = p->message.topic.topic.utf8;
		// coap_cloud_evt.data.msg.topic.len = p->message.topic.topic.size;

		coap_cloud_notify_event(&coap_cloud_evt);
#endif

	// } break;
	// case COAP_EVT_PUBACK:
	// 	LOG_DBG("COAP_EVT_PUBACK: id = %d result = %d",
	// 		coap_evt->param.puback.message_id,
	// 		coap_evt->result);
	// 	break;
	// case COAP_EVT_SUBACK:
	// 	LOG_DBG("COAP_EVT_SUBACK: id = %d result = %d",
	// 		coap_evt->param.suback.message_id,
	// 		coap_evt->result);
	// 	break;
	// default:
	// 	break;
	// }
}

static struct sockaddr_in parse_ip_address(int port, char *ip_address) {
  struct sockaddr_in addr;
	// struct sockaddr_storage addr;

  memset(&addr, '0', sizeof(addr)); // init to zero

  // convert to usable address
  if (inet_pton(AF_INET, ip_address, &addr.sin_addr) <= 0) {
    printf("\n IP parsing failed\n");
    exit(EXIT_FAILURE);
  }

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  return addr;
}

// #if defined(CONFIG_COAP_CLOUD_STATIC_IPV4)
// static int broker_init(void)
// {
//
// 	printk("hello from broker_init\n");
// 	struct sockaddr_in *broker4 =
// 		((struct sockaddr_in *)&broker);
//
// 	inet_pton(AF_INET, CONFIG_COAP_CLOUD_STATIC_IPV4_ADDR,
// 		  &broker->sin_addr);
// 	broker4->sin_family = AF_INET;
// 	broker4->sin_port = htons(CONFIG_COAP_CLOUD_PORT);
//
// 	LOG_DBG("IPv4 Address %s", log_strdup(CONFIG_COAP_CLOUD_STATIC_IPV4_ADDR));
//
// 	return 0;
// }
// #else
static int broker_init(void) {

	int err;
	// struct addrinfo *result;
	// struct addrinfo *addr;
	// struct addrinfo hints = {
	// 	.ai_family = AF_INET,
	// 	.ai_socktype = SOCK_STREAM
	// };

	// printf("trying to connect to host: %s\n", CONFIG_COAP_CLOUD_BROKER_HOST_NAME);


	// err = getaddrinfo(CONFIG_COAP_CLOUD_BROKER_HOST_NAME,
	// 		  NULL, &hints, &result);
	// if (err) {
	// 	LOG_ERR("getaddrinfo, error %d", err);
	// 	return -ECHILD;
	// }

	// addr = result;

	// while (addr != NULL) {

	printk("in broker_init\n");

		char *ip_address = "83.150.54.152";
		broker = parse_ip_address(5683, ip_address);

		sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); //iproto_ip
	  if (sock < 0) {
	    printk("Failed to create CoAP socket: %d.\n", errno);
	    return -errno;
	  }

		err = connect(sock, (struct sockaddr *)&broker,
	      sizeof(struct sockaddr_in));
	  if (err < 0) {
	    printk("Connect failed : %d\n", errno);
	    return -errno;
	  }



		// if ((addr->ai_addrlen == sizeof(struct sockaddr_in)) &&
		    // (AWS_AF_FAMILY == AF_INET)) {
			// struct sockaddr_in *broker4 = ((struct sockaddr_in *)&broker);
			// char ipv4_addr[NET_IPV4_ADDR_LEN];
			//
			// broker4->sin_addr.s_addr = ((struct sockaddr_in *)addr->ai_addr)->sin_addr.s_addr;
			// broker4->sin_family = AF_INET;
			// broker4->sin_port = htons(CONFIG_COAP_CLOUD_PORT);
			//
			// inet_ntop(AF_INET, &broker4->sin_addr.s_addr, ipv4_addr,
			// 	  sizeof(ipv4_addr));
			// LOG_DBG("IPv4 Address found %s", log_strdup(ipv4_addr));
			// break;
		// } else if ((addr->ai_addrlen == sizeof(struct sockaddr_in6)) &&
		// 	   (AWS_AF_FAMILY == AF_INET6)) {
		// 	struct sockaddr_in6 *broker6 =
		// 		((struct sockaddr_in6 *)&broker);
		// 	char ipv6_addr[NET_IPV6_ADDR_LEN];
		//
		// 	memcpy(broker6->sin6_addr.s6_addr,
		// 	       ((struct sockaddr_in6 *)addr->ai_addr)
		// 	       ->sin6_addr.s6_addr,
		// 	       sizeof(struct in6_addr));
		// 	broker6->sin6_family = AF_INET6;
		// 	broker6->sin6_port = htons(CONFIG_COAP_CLOUD_PORT);
		//
		// 	inet_ntop(AF_INET6, &broker6->sin6_addr.s6_addr,
		// 		  ipv6_addr, sizeof(ipv6_addr));
		// 	LOG_DBG("IPv4 Address found %s", log_strdup(ipv6_addr));
		// 	break;
		// }

		// LOG_DBG("ai_addrlen = %u should be %u or %u",
		// 	(unsigned int)addr->ai_addrlen,
		// 	(unsigned int)sizeof(struct sockaddr_in),
		// 	(unsigned int)sizeof(struct sockaddr_in6));
		//
		// addr = addr->ai_next;
		// break;
	// }

	// freeaddrinfo(result);
  printk("Created broker sock: %d\n", sock);


  // init_coap_client
	client.sock = sock;
	client.broker = &broker;


	// should send ready event
	printk("now gonna trigger COAP_CLOUD_EVENT_CONNECTED: %d\n", CLOUD_EVT_CONNECTED);
	struct cloud_backend_config *config = coap_cloud_backend->config;
	struct cloud_event cloud_evt = { CLOUD_EVT_READY };
  cloud_notify_event(coap_cloud_backend, &cloud_evt, config->user_data);


	return err;
}
// #endif



static int client_broker_init(void) //struct coap_client *const client) // TODO add client here struct coap_client *const client)
{
	int err = 0;
	//
	// coap_client_init(&client); this is done in broker init
	//
	err = broker_init();
	// printk("got to brokerInit\n");
	// if (err) {
	// 	return err;
	// }

  // pass the socket
	// client.broker			= &broker;
	// pass the event handler
	// client->event_handler			= coap_evt_handler;

	// OLD stuff for mqtt client
	// client->client_id.utf8		= (char *)client_id_buf;
	// client->client_id.size		= strlen(client_id_buf);
	// client->password		= NULL;
	// client->user_name		= NULL;
	// client->protocol_version	= COAP_VERSION_3_1_1;
	// client->rx_buf			= rx_buffer;
	// client->rx_buf_size		= sizeof(rx_buffer);
	// client->tx_buf			= tx_buffer;
	// client->tx_buf_size		= sizeof(tx_buffer);
	// client->transport.type		= COAP_TRANSPORT_NON_SECURE;

// #if defined(CONFIG_COAP_CLOUD_PERSISTENT_SESSIONS)
// 	client->clean_session		= 0U;
// #endif

	// static sec_tag_t sec_tag_list[] = { CONFIG_COAP_CLOUD_SEC_TAG };
	// struct mqtt_sec_config *tls_cfg = &(client->transport).tls.config;
	//
	// tls_cfg->peer_verify		= 2;
	// tls_cfg->cipher_count		= 0;
	// tls_cfg->cipher_list		= NULL;
	// tls_cfg->sec_tag_count		= ARRAY_SIZE(sec_tag_list);
	// tls_cfg->sec_tag_list		= sec_tag_list;
	// tls_cfg->hostname		= CONFIG_COAP_CLOUD_BROKER_HOST_NAME;

	return err;
}

static int connection_poll_start(void)
{
	// if (atomic_get(&connection_poll_active)) {
	// 	LOG_DBG("Connection poll in progress");
	// 	return -EINPROGRESS;
	// }

	printk("inside connection_poll_start");

	atomic_set(&disconnect_requested, 0);
	k_sem_give(&connection_poll_sem);

	return 0;
}

static int connect_error_translate(const int err)
{
	switch (err) {
#if defined(CONFIG_CLOUD_API)
	case 0:
		return CLOUD_CONNECT_RES_SUCCESS;
	case -ECHILD:
		return CLOUD_CONNECT_RES_ERR_NETWORK;
	case -EACCES:
		return CLOUD_CONNECT_RES_ERR_NOT_INITD;
	case -ENOEXEC:
		return CLOUD_CONNECT_RES_ERR_BACKEND;
	case -EINVAL:
		return CLOUD_CONNECT_RES_ERR_PRV_KEY;
	case -EOPNOTSUPP:
		return CLOUD_CONNECT_RES_ERR_CERT;
	case -ECONNREFUSED:
		return CLOUD_CONNECT_RES_ERR_CERT_MISC;
	case -ETIMEDOUT:
		return CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA;
	case -ENOMEM:
		return CLOUD_CONNECT_RES_ERR_NO_MEM;
	case -EINPROGRESS:
		return CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED;
	default:
		LOG_ERR("AWS IoT backend connect failed %d", err);
		return CLOUD_CONNECT_RES_ERR_MISC;
#else
	case 0:
		return COAP_CLOUD_CONNECT_RES_SUCCESS;
	case -ECHILD:
		return COAP_CLOUD_CONNECT_RES_ERR_NETWORK;
	case -EACCES:
		return COAP_CLOUD_CONNECT_RES_ERR_NOT_INITD;
	case -ENOEXEC:
		return COAP_CLOUD_CONNECT_RES_ERR_BACKEND;
	case -EINVAL:
		return COAP_CLOUD_CONNECT_RES_ERR_PRV_KEY;
	case -EOPNOTSUPP:
		return COAP_CLOUD_CONNECT_RES_ERR_CERT;
	case -ECONNREFUSED:
		return COAP_CLOUD_CONNECT_RES_ERR_CERT_MISC;
	case -ETIMEDOUT:
		return COAP_CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA;
	case -ENOMEM:
		return COAP_CLOUD_CONNECT_RES_ERR_NO_MEM;
	case -EINPROGRESS:
		return COAP_CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED;
	default:
		LOG_ERR("AWS broker connect failed %d", err);
		return CLOUD_CONNECT_RES_ERR_MISC;
#endif
	}
}

int coap_cloud_ping(void)
{
	// return mqtt_ping(&client);
	return 0;
}

int coap_cloud_keepalive_time_left(void)
{
	return 5; // (int)mqtt_keepalive_time_left(&client);
}

int coap_cloud_input(void)
{
	return 0; //mqtt_input(&client);
}

int coap_cloud_send(char* string)//const struct coap_cloud_data *const data) //  *const tx_data)
{

  LOG_INF("sending (not really): %s\n", string);
	return 0; // mqtt_publish(&client, &param);
}

int coap_cloud_disconnect(void)
{
	atomic_set(&disconnect_requested, 1);
	// return coap_disconnect(&client);
	return 0;
}

int coap_cloud_connect(struct coap_cloud_config *const config)
{
	int err;

	// printk("now in coap cloud connect\n");

	if (IS_ENABLED(CONFIG_COAP_CLOUD_CONNECTION_POLL_THREAD)) {
    // if(1){
		err = connection_poll_start();
		printk("got to connection_poll_start\n");
	} else {
		// printk("landed in else of poll_start\n");
		atomic_set(&disconnect_requested, 0);
		// printk("now with client_broker_init start\n");
		LOG_INF("calling client_broker_init (cloud: 910)");
		err = client_broker_init();

		if (err) {
			LOG_ERR("client_broker_init, error: %d", err);
			return err;
		}
    // TODO send a connack / CLOUD_CONNECT_RES_SUCCESS
		// err = mqtt_connect(&client);
		// err = coap_connect();
		// if (err) {
		// 	LOG_ERR("coap_connect, error: %d", err);
		// }

		err = connect_error_translate(err);

#if !defined(CONFIG_CLOUD_API)
                // no need for tls TODO maybe should add socket here..
		config->socket = client.transport.tls.sock;
		// config->socket = sock;
#endif
	}

  printk("error in coap_cloud_connect: %d\n", err);

  //struct cloud_backend_config *config = coap_cloud_backend->config;
	//struct cloud_event cloud_evt = { 0 };
	//cloud_evt.type = CLOUD_EVT_CONNECTED;


	return err;
}

// int mqtt_cloud_subscription_topics_add(
// 			const struct mqtt_cloud_topic_data *const topic_list,
// 			size_t list_count)
// {
// 	if (list_count == 0) {
// 		LOG_ERR("Application subscription list is 0");
// 		return -EMSGSIZE;
// 	}
//
// 	if (list_count != CONFIG_COAP_CLOUD_APP_SUBSCRIPTION_LIST_COUNT) {
// 		LOG_ERR("Application subscription list count mismatch");
// 		return -EMSGSIZE;
// 	}
//
// 	for (size_t i = 0; i < list_count; i++) {
// 		app_topic_data.list[i].topic.utf8 = topic_list[i].str;
// 		app_topic_data.list[i].topic.size = topic_list[i].len;
// 		app_topic_data.list[i].qos = COAP_QOS_1_AT_LEAST_ONCE;
// 	}
//
// 	app_topic_data.list_count = list_count;
//
// 	return 0;
// }


int coap_cloud_init(const struct coap_cloud_config *const config,
		 coap_cloud_evt_handler_t event_handler)
{

	int err;


#if !defined(CONFIG_CLOUD_API)
printk("in coap_cloud_init, CONFIG_CLOUD_API is not defined\n");
	module_evt_handler = event_handler;
#endif

	return 0; //err
}

// TODO might need this later
#if defined(CONFIG_COAP_CLOUD_CONNECTION_POLL_THREAD)
void mqtt_cloud_cloud_poll(void)
{

	printk("Started POLLING....\n");
	int err;
	struct pollfd fds[1];
#if defined(CONFIG_CLOUD_API)
	struct cloud_event cloud_evt = {
		.type = CLOUD_EVT_DISCONNECTED,
		.data = { .err = CLOUD_DISCONNECT_MISC}
	};
#else
	struct coap_cloud_evt cloud_evt = {
		.type = COAP_CLOUD_EVT_DISCONNECTED,
		.data = { .err = COAP_CLOUD_DISCONNECT_MISC}
	};
#endif

start:
  printk("POLL ::START::\n");
	k_sem_take(&connection_poll_sem, K_FOREVER);
	atomic_set(&connection_poll_active, 1);

#if defined(CONFIG_CLOUD_API)
	cloud_evt.data.err = CLOUD_CONNECT_RES_SUCCESS;
	cloud_evt.type = CLOUD_EVT_CONNECTING;
	cloud_notify_event(coap_cloud_backend, &cloud_evt, NULL);
#else
	cloud_evt.data.err = COAP_CLOUD_CONNECT_RES_SUCCESS;
	cloud_evt.type = COAP_CLOUD_EVT_CONNECTING;
	coap_cloud_notify_event(&cloud_evt);
#endif

	// err = client_broker_init(&client);
	if (err) {
		LOG_ERR("client_broker_init, error: %d", err);
	}

	// err = mqtt_connect(&client);
	// set coap connection
	// err = coap_cloud_connect(&client);
	err = 0;
	if (err) {
		LOG_ERR("mqtt_connect, error: %d", err);
	}

	printk("got error in event handler: %d\n", err);

	err = connect_error_translate(err);

#if defined(CONFIG_CLOUD_API)
	if (err != CLOUD_CONNECT_RES_SUCCESS) {
		cloud_evt.data.err = err;
		cloud_evt.type = CLOUD_EVT_CONNECTING;
		cloud_notify_event(coap_cloud_backend, &cloud_evt, NULL);
		goto reset;
	} else {
		LOG_DBG("Cloud connection request sent.");
	}
#else
	if (err != COAP_CLOUD_CONNECT_RES_SUCCESS) {
		cloud_evt.data.err = err;
		cloud_evt.type = COAP_CLOUD_EVT_CONNECTING;
		coap_cloud_notify_event(&cloud_evt);
		goto reset;
	} else {
		LOG_DBG("AWS broker connection request sent.");
	}
#endif
  // coap_client struct has no member transport etc, only a sock
	//fds[0].fd = client.transport.tls.sock;
	fds[0].fd = client.sock;
	fds[0].events = POLLIN;

	/* Only disconnect events will occur below */
#if defined(CONFIG_CLOUD_API)
	cloud_evt.type = CLOUD_EVT_DISCONNECTED;
#else
	cloud_evt.type = COAP_CLOUD_EVT_DISCONNECTED;
#endif

	while (true) {
		printk("in while of poll");
		err = poll(fds, ARRAY_SIZE(fds), COAP_CLOUD_POLL_TIMEOUT_MS);

		if (err == 0) {
			if (coap_cloud_keepalive_time_left() <
			    COAP_CLOUD_POLL_TIMEOUT_MS) {
				coap_cloud_ping();
			}
			continue;
		}

		if ((fds[0].revents & POLLIN) == POLLIN) {
			coap_cloud_input();
			continue;
		}

		if (err < 0) {
			LOG_ERR("poll() returned an error: %d", err);
#if defined(CONFIG_CLOUD_API)
			cloud_evt.data.err = CLOUD_DISCONNECT_MISC;
#else
			cloud_evt.data.err = COAP_CLOUD_DISCONNECT_MISC;
#endif
			break;
		}

		if (atomic_get(&disconnect_requested)) {
			atomic_set(&disconnect_requested, 0);
			LOG_DBG("Expected disconnect event.");
#if defined(CONFIG_CLOUD_API)
			cloud_evt.data.err = CLOUD_DISCONNECT_USER_REQUEST;
			cloud_notify_event(coap_cloud_backend, &cloud_evt, NULL);
#else
			cloud_evt.data.err = COAP_CLOUD_DISCONNECT_MISC;
			coap_cloud_notify_event(&cloud_evt);
#endif
			goto reset;
		}

		if ((fds[0].revents & POLLNVAL) == POLLNVAL) {
			LOG_DBG("Socket error: POLLNVAL");
			LOG_DBG("The cloud socket was unexpectedly closed.");
#if defined(CONFIG_CLOUD_API)
			cloud_evt.data.err = CLOUD_DISCONNECT_INVALID_REQUEST;
#else
			cloud_evt.data.err = COAP_CLOUD_DISCONNECT_INVALID_REQUEST;
#endif
			break;
		}

		if ((fds[0].revents & POLLHUP) == POLLHUP) {
			LOG_DBG("Socket error: POLLHUP");
			LOG_DBG("Connection was closed by the cloud.");
#if defined(CONFIG_CLOUD_API)
			cloud_evt.data.err = CLOUD_DISCONNECT_CLOSED_BY_REMOTE;
#else
			cloud_evt.data.err =
					COAP_CLOUD_DISCONNECT_CLOSED_BY_REMOTE;
#endif
			break;
		}

		if ((fds[0].revents & POLLERR) == POLLERR) {
			LOG_DBG("Socket error: POLLERR");
			LOG_DBG("Cloud connection was unexpectedly closed.");
#if defined(CONFIG_CLOUD_API)
			cloud_evt.data.err = CLOUD_DISCONNECT_MISC;
#else
			cloud_evt.data.err = COAP_CLOUD_DISCONNECT_MISC;
#endif
			break;
		}
	}

#if defined(CONFIG_CLOUD_API)
	cloud_notify_event(coap_cloud_backend, &cloud_evt, NULL);
#else
	coap_cloud_notify_event(&cloud_evt);
#endif
	coap_cloud_disconnect();

reset:
	atomic_set(&connection_poll_active, 0);
	k_sem_take(&connection_poll_sem, K_NO_WAIT);
	goto start;
}

#ifdef CONFIG_BOARD_QEMU_X86
#define POLL_THREAD_STACK_SIZE 4096
#else
#define POLL_THREAD_STACK_SIZE 2560
#endif

K_THREAD_DEFINE(connection_poll_thread, POLL_THREAD_STACK_SIZE,
		mqtt_cloud_cloud_poll, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
#endif

#if defined(CONFIG_CLOUD_API)
static int c_init(const struct cloud_backend *const backend,
		  cloud_evt_handler_t handler)
{

  LOG_INF("2 -- in c_init");
	#if !defined(CONFIG_CLOUD_API)
		module_evt_handler = event_handler;
	#endif
	backend->config->handler = handler;
	coap_cloud_backend = (struct cloud_backend *)backend;

	struct coap_cloud_config config = {
		.client_id = backend->config->id,
		.client_id_len = backend->config->id_len
	};
	return coap_cloud_init(&config, NULL);
}

static int c_ep_subscriptions_add(const struct cloud_backend *const backend,
				  const struct cloud_endpoint *const list,
				  size_t list_count)
{
	// struct mqtt_cloud_topic_data topic_list[list_count];

	// for (size_t i = 0; i < list_count; i++) {
	// 	topic_list[i].str = list[i].str;
	// 	topic_list[i].len = list[i].len;
	// }

	return 0; // mqtt_cloud_subscription_topics_add(topic_list, list_count);
}

static int c_connect(const struct cloud_backend *const backend)
{
	LOG_INF("c_connect called (cloud: 1195)");
	return coap_cloud_connect(backend);
}

static int c_disconnect(const struct cloud_backend *const backend)
{
	return coap_cloud_disconnect();
}

static int c_send(const struct cloud_backend *const backend,
		  const struct cloud_msg *const msg)
{

	printk("C_SEND called\n");

	return coap_cloud_send("string");
}

static int c_input(const struct cloud_backend *const backend)
{
	return coap_cloud_input();
}

static int c_ping(const struct cloud_backend *const backend)
{
	printk("c_ping called\n");
	return coap_cloud_ping();
}

static int c_keepalive_time_left(const struct cloud_backend *const backend)
{
	return 5; // coap_cloud_keepalive_time_left();
}

static const struct cloud_api coap_cloud_api = {
	.init			    = c_init,
	.connect		  = c_connect,
	.disconnect		= c_disconnect,
	.send		    	= c_send,
	.ping		    	= c_ping,
	.keepalive_time_left	= c_keepalive_time_left,
	.input			  = c_input,
	.ep_subscriptions_add	= c_ep_subscriptions_add
};


CLOUD_BACKEND_DEFINE(COAP_CLOUD, coap_cloud_api);
#endif
