#include "coap_cloud.h"
#include <net/mqtt.h>
#include <net/socket.h>
#include <net/cloud.h>
#include <random/rand32.h>
#include <stdio.h>
#include <net/coap_utils.h>
#include <net/coap.h>

// #if defined(CONFIG_AWS_FOTA)
// #include <net/aws_fota.h>
// #endif

#include <logging/log.h>

LOG_MODULE_REGISTER(coap_cloud, CONFIG_COAP_CLOUD_LOG_LEVEL);


// COAP stuff
#define APP_COAP_SEND_INTERVAL_MS 5000
#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1

// registered resources
static const char * const obs_path[] = { "obs", NULL };

static const char * const test_path[] = { "testing", NULL };


// TODO add to config files
#define CONFIG_COAP_RESOURCE "other/block"// "obs"
#define CONFIG_COAP_SERVER_HOSTNAME "83.150.54.152"
  //"californium.eclipse.org"
// #define MESSAGE_ID u16_t

static struct coap_client client;
// static struct sockaddr_storage broker;
static struct sockaddr_in broker; // TODO use ipv6 eventually
static int sock;
static u16_t next_token;
#define MESSAGE_ID next_token
static u8_t coap_buf[APP_COAP_MAX_MSG_LEN];


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


// #if defined(CONFIG_AWS_FOTA)
// static void aws_fota_cb_handler(struct aws_fota_event *fota_evt)
// {
// #if defined(CONFIG_CLOUD_API)
// 	struct cloud_backend_config *config = coap_cloud_backend->config;
// 	struct cloud_event cloud_evt = { 0 };
// #else
// 	struct coap_cloud_evt coap_cloud_evt = { 0 };
// #endif
//
// 	if (fota_evt == NULL) {
// 		return;
// 	}
//
// 	switch (fota_evt->id) {
// 	case AWS_FOTA_EVT_START:
// 		LOG_DBG("AWS_FOTA_EVT_START");
// #if defined(CONFIG_CLOUD_API)
// 		cloud_evt.type = CLOUD_EVT_FOTA_START;
// 		cloud_notify_event(coap_cloud_backend, &cloud_evt,
// 				   config->user_data);
// #else
// 		coap_cloud_evt.type = COAP_CLOUD_EVT_FOTA_START;
// 		coap_cloud_notify_event(&coap_cloud_evt);
// #endif
// 		break;
// 	case AWS_FOTA_EVT_DONE:
// 		LOG_DBG("AWS_FOTA_EVT_DONE");
// #if defined(CONFIG_CLOUD_API)
// 		cloud_evt.type = CLOUD_EVT_FOTA_DONE;
// 		cloud_notify_event(coap_cloud_backend, &cloud_evt,
// 				   config->user_data);
// #else
// 		coap_cloud_evt.type = COAP_CLOUD_EVT_FOTA_DONE;
// 		coap_cloud_notify_event(&coap_cloud_evt);
// #endif
// 		break;
// 	case AWS_FOTA_EVT_ERASE_PENDING:
// 		LOG_DBG("AWS_FOTA_EVT_ERASE_PENDING");
// #if defined(CONFIG_CLOUD_API)
// 		cloud_evt.type = CLOUD_EVT_FOTA_ERASE_PENDING;
// 		cloud_notify_event(coap_cloud_backend, &cloud_evt,
// 				   config->user_data);
// #else
// 		coap_cloud_evt.type = COAP_CLOUD_EVT_FOTA_ERASE_PENDING;
// 		coap_cloud_notify_event(&coap_cloud_evt);
// #endif
// 		break;
// 	case AWS_FOTA_EVT_ERASE_DONE:
// 		LOG_DBG("AWS_FOTA_EVT_ERASE_DONE");
// #if defined(CONFIG_CLOUD_API)
// 		cloud_evt.type = CLOUD_EVT_FOTA_ERASE_DONE;
// 		cloud_notify_event(coap_cloud_backend, &cloud_evt,
// 				   config->user_data);
// #else
// 		coap_cloud_evt.type = COAP_CLOUD_EVT_FOTA_ERASE_DONE;
// 		coap_cloud_notify_event(&coap_cloud_evt);
// #endif
// 		break;
// 	case AWS_FOTA_EVT_ERROR:
// 		LOG_ERR("AWS_FOTA_EVT_ERROR");
// 		break;
// 	case AWS_FOTA_EVT_DL_PROGRESS:
// 		LOG_DBG("AWS_FOTA_EVT_DL_PROGRESS");
// 		break;
// 	default:
// 		LOG_ERR("Unknown FOTA event");
// 		break;
// 	}
// }
// #endif



// Zephyr code ================= start ====================

static int send_simple_coap_request(uint8_t method)
{
	uint8_t payload[] = "payload";
	struct coap_packet request;
	const char * const *p;
	uint8_t *data;
	int r;

	data = (uint8_t *)k_malloc(APP_COAP_MAX_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	r = coap_packet_init(&request, data, APP_COAP_MAX_MSG_LEN,
			     1, COAP_TYPE_CON, 8, coap_next_token(),
			     method, coap_next_id());
	if (r < 0) {
		LOG_ERR("Failed to init CoAP message");
		goto end;
	}

	for (p = test_path; p && *p; p++) {
		r = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
					      *p, strlen(*p));
		if (r < 0) {
			LOG_ERR("Unable add option to request");
			goto end;
		}
	}

	switch (method) {
	case COAP_METHOD_GET:
	case COAP_METHOD_DELETE:
		break;

	case COAP_METHOD_PUT:
	case COAP_METHOD_POST:
		r = coap_packet_append_payload_marker(&request);
		if (r < 0) {
			LOG_ERR("Unable to append payload marker");
			goto end;
		}

		r = coap_packet_append_payload(&request, (uint8_t *)payload,
					       sizeof(payload) - 1);
		if (r < 0) {
			LOG_ERR("Not able to append payload");
			goto end;
		}

		break;
	default:
		r = -EINVAL;
		goto end;
	}

	net_hexdump("Request", request.data, request.offset);

	r = send(sock, request.data, request.offset, 0);

end:
	k_free(data);

	return 0;
}

static int send_simple_coap_msgs_and_wait_for_reply(void)
{
	uint8_t test_type = 0U;
	int r;

	while (1) {
		switch (test_type) {
		case 0:
			/* Test CoAP GET method */
			printk("\nCoAP client GET\n");
			r = send_simple_coap_request(COAP_METHOD_GET);
			if (r < 0) {
				return r;
			}

			break;
		case 1:
			/* Test CoAP PUT method */
			printk("\nCoAP client PUT\n");
			r = send_simple_coap_request(COAP_METHOD_PUT);
			if (r < 0) {
				return r;
			}

			break;
		case 2:
			/* Test CoAP POST method*/
			printk("\nCoAP client POST\n");
			r = send_simple_coap_request(COAP_METHOD_POST);
			if (r < 0) {
				return r;
			}

			break;
		case 3:
			/* Test CoAP DELETE method*/
			printk("\nCoAP client DELETE\n");
			r = send_simple_coap_request(COAP_METHOD_DELETE);
			if (r < 0) {
				return r;
			}

			break;
		default:
			return 0;
		}

		r = process_simple_coap_reply();
		if (r < 0) {
			return r;
		}

		test_type++;
	}

	return 0;
}

static int send_obs_coap_request(void)
{
	struct coap_packet request;
	const char * const *p;
	uint8_t *data;
	int r;

	data = (uint8_t *)k_malloc(APP_COAP_MAX_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	r = coap_packet_init(&request, data, APP_COAP_MAX_MSG_LEN,
			     1, COAP_TYPE_CON, 8, coap_next_token(),
			     COAP_METHOD_GET, coap_next_id());
	if (r < 0) {
		LOG_ERR("Failed to init CoAP message");
		goto end;
	}

	r = coap_append_option_int(&request, COAP_OPTION_OBSERVE, 0);
	if (r < 0) {
		LOG_ERR("Failed to append Observe option");
		goto end;
	}

	for (p = obs_path; p && *p; p++) {
		r = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
					      *p, strlen(*p));
		if (r < 0) {
			LOG_ERR("Unable add option to request");
			goto end;
		}
	}

	net_hexdump("Request", request.data, request.offset);

	r = send(sock, request.data, request.offset, 0);

end:
	k_free(data);

	return r;
}

static int process_obs_coap_reply(void)
{
	struct coap_packet reply;
	uint16_t id;
	uint8_t token[8];
	uint8_t *data;
	uint8_t type;
	uint8_t tkl;
	int rcvd;
	int ret;

	wait();

	data = (uint8_t *)k_malloc(APP_COAP_MAX_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	rcvd = recv(sock, data, APP_COAP_MAX_MSG_LEN, MSG_DONTWAIT);
	if (rcvd == 0) {
		ret = -EIO;
		goto end;
	}

	if (rcvd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			ret = 0;
		} else {
			ret = -errno;
		}

		goto end;
	}

	net_hexdump("Response", data, rcvd);

	ret = coap_packet_parse(&reply, data, rcvd, NULL, 0);
	if (ret < 0) {
		LOG_ERR("Invalid data received");
		goto end;
	}

	tkl = coap_header_get_token(&reply, (uint8_t *)token);
	id = coap_header_get_id(&reply);

	type = coap_header_get_type(&reply);
	if (type == COAP_TYPE_ACK) {
		ret = 0;
	} else if (type == COAP_TYPE_CON) {
		ret = send_obs_reply_ack(id, token, tkl);
	}
end:
	k_free(data);

	return ret;
}


// TODO do this in a seperate thread
static int register_observer(void)
{
	uint8_t counter = 0U;
	int r;

	while (1) {
		/* Test CoAP OBS GET method */
		if (!counter) {
			printk("\nCoAP client OBS GET\n");
			r = send_obs_coap_request();
			if (r < 0) {
				return r;
			}
		} else {
			printk("\nCoAP OBS Notification\n");
		}

		r = process_obs_coap_reply();
		if (r < 0) {
			return r;
		}

		counter++;

		/* Unregister */
		if (counter == 5U) {
			/* TODO: Functionality can be verified byt waiting for
			 * some time and make sure client shouldn't receive
			 * any notifications. If client still receives
			 * notifications means, Observer is not removed.
			 */
			return send_obs_reset_coap_request();
		}
	}

	return 0;
}

// Zephyr code ================= end ====================

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

// #if defined(CONFIG_AWS_FOTA)
// 	err = aws_fota_mqtt_evt_handler(c, coap_evt);
// 	if (err == 0) {
// 		/* Event handled by FOTA library so it can be skipped. */
// 		return;
// 	} else if (err < 0) {
// 		LOG_ERR("aws_fota_mqtt_evt_handler, error: %d", err);
// 		LOG_DBG("Disconnecting COAP client...");
//
// 		atomic_set(&disconnect_requested, 1);
// 		err = coap_disconnect(c);
// 		if (err) {
// 			LOG_ERR("Could not disconnect: %d", err);
// 		}
// 	}
// #endif

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

/* helper function to parse ip adresses to sockaddr_in struct */
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


static int broker_init(void) {

	int err;

	char *ip_address = CONFIG_COAP_SERVER_HOSTNAME; // "83.150.54.152";
	broker = parse_ip_address(5683, ip_address);

	// initialize socket for coap message sending
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	 if (sock < 0) {
	   LOG_ERR("Failed to create CoAP socket: %d.\n", errno);
	   return -errno;
	 }

		err = connect(sock, (struct sockaddr *)&broker, sizeof(struct sockaddr_in));
	  if (err < 0) {
	    printk("Connect failed : %d\n", errno);
	    return -errno;
	  }


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

int coap_cloud_keepalive_time_left(void)
{
	return 5; // (int)mqtt_keepalive_time_left(&client);
}

int coap_cloud_input(void)
{
	return 0; //mqtt_input(&client);
}


/* EXPERIMENTAL */
// static int client_get_send(void)
// {
// 	int err;
// 	struct coap_packet request;
//
//
// 	next_token++;
//
// 	err = coap_packet_init(&request, coap_buf, sizeof(coap_buf),
// 			       APP_COAP_VERSION, COAP_TYPE_NON_CON,
// 			       sizeof(next_token), (u8_t *)&next_token,
// 			       COAP_METHOD_GET, MESSAGE_ID); // coap_next_id());
// 	if (err < 0) {
// 		printk("Failed to create CoAP request, %d\n", err);
// 		return err;
// 	}
//
// 	err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
// 					(u8_t *)CONFIG_COAP_RESOURCE,
// 					strlen(CONFIG_COAP_RESOURCE));
// 	if (err < 0) {
// 		printk("Failed to encode CoAP option, %d\n", err);
// 		return err;
// 	}
//
// 	err = send(sock, request.data, request.offset, 0);
// 	if (err < 0) {
// 		printk("Failed to send CoAP request, %d\n", errno);
// 		return -errno;
// 	}
//
// 	printk("CoAP request sent: token 0x%04x\n", next_token);
//
// 	return 0;
// }

// int coap_cloud_send(const struct cloud_msg *const msg)//const struct coap_cloud_data *const data) //  *const tx_data)
// {

	// LOG_INF("MESSAGE: %s\n", msg->buf);
  // // client_get_send();
	//
	//
  // // coap_init(AF_INET);
	//
	//
	//
	// // coap_init(AF_INET);
	//
	// char *path = "testing";
	// int err;
	// struct coap_packet request;
	// uint8_t data[100];
	// uint8_t payload[20];
	//
	// coap_packet_init(&request, data, sizeof(data),
  //                1, COAP_TYPE_NON_CON, 8, coap_next_token(),
  //                COAP_METHOD_PUT, coap_next_id());
	//
  // /* Append options */
	// // err = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
  // //     (u8_t *)CONFIG_COAP_RESOURCE,
  // //     strlen(CONFIG_COAP_RESOURCE));
  // coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
  //                         path, strlen(path));
	//
  // /* Append Payload marker if you are going to add payload */
  // coap_packet_append_payload_marker(&request);
	//
  // /* Append payload */
  // coap_packet_append_payload(&request, (uint8_t *)payload,
  //                          sizeof(payload) - 1);
	//
  // err = send(sock, request.data, request.offset, 0);
	// if (err < 0) {
	//   printk("Failed to send CoAP request, %d\n", errno);
	//   return -errno;
  // }
	//
	// printk("CoAP request sent: token 0x%04x\n", next_token);
	//
	// return 0; // mqtt_publish(&client, &param);
// }

int coap_cloud_disconnect(void)
{
	atomic_set(&disconnect_requested, 1);
	// return coap_disconnect(&client);
	return 0;
}

// POLLING activated here
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

int coap_cloud_init(const struct coap_cloud_config *const config,
		 coap_cloud_evt_handler_t event_handler)
{
	int err;

  // token for coap testing, can be removed if not used anywhere anymore
	next_token = sys_rand32_get();


#if !defined(CONFIG_CLOUD_API)
printk("in coap_cloud_init, CONFIG_CLOUD_API is not defined\n");
	module_evt_handler = event_handler;
#endif

	return 0; //err
}

// TODO might need this later
#if defined(CONFIG_COAP_CLOUD_CONNECTION_POLL_THREAD)
void coap_cloud_cloud_poll(void)
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
						// todo cant call cping
				// c_ping();
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
		coap_cloud_cloud_poll, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
#endif

#if defined(CONFIG_CLOUD_API)
static int c_init(const struct cloud_backend *const backend,
		  cloud_evt_handler_t handler)
{
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
	return 0;
}

static int c_connect(const struct cloud_backend *const backend)
{
	return coap_cloud_connect(backend);
}

static int c_disconnect(const struct cloud_backend *const backend)
{
	return coap_cloud_disconnect();
}

static int c_send(const struct cloud_backend *const backend,
		  const struct cloud_msg *const msg)
{
	LOG_INF("C_SEND called, MESSAGE: %s\n", msg->buf);

	int err;

  // path to resource on server
	char *path = "testing";
	struct coap_packet request;
	uint8_t data[100];

  /* copy asset_tracker msg to coap data buffer */
	uint8_t payload[strlen(msg->buf)];
  strcpy(payload, msg->buf);

  /* initialize coap packet */
	err = coap_packet_init(&request, data, sizeof(data),
								 1, COAP_TYPE_NON_CON, 8, coap_next_token(),
								 COAP_METHOD_PUT, coap_next_id());
	if (err < 0) {
		LOG_ERR("Failed to initialize coap packet, %d\n", errno);
		return -errno;
 	}

  /* append options */
	coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
													path, strlen(path));
	if (err < 0) {
		LOG_ERR("Failed to append coap packet options, %d\n", errno);
		return -errno;
	}

	/* Append Payload marker if going to add payload */
	coap_packet_append_payload_marker(&request);

	/* Append payload (data received from asset_tracker) */
	coap_packet_append_payload(&request, (uint8_t *)payload,
													 sizeof(payload) - 1);

	err = send(sock, request.data, request.offset, 0);
	if (err < 0) {
		LOG_ERR("Failed to send CoAP request, %d\n", errno);
		return -errno;
	}

	LOG_INF("request content: %s\n", payload);

	// TODO free payload?

	// LOG_INF("err: %d\n", err);

	return 0;
}

static int c_input(const struct cloud_backend *const backend)
{
	return coap_cloud_input();
}

/* called periodically to keep connection to broker alive
 * @return 0 if successful, error code otherwies */
static int c_ping()
{
	printk("c_ping called\n");
	return 0;
}
// TODO
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
