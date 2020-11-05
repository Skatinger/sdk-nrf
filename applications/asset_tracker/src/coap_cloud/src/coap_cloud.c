#include "coap_cloud.h"
#include <net/mqtt.h>
#include <net/socket.h>
#include <nrf_socket.h>
#include <net/cloud.h>
#include <random/rand32.h>
#include <stdio.h>
#include <net/coap_utils.h>
#include <net/coap.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(coap_cloud, CONFIG_COAP_CLOUD_LOG_LEVEL);

// COAP stuff
#define APP_COAP_SEND_INTERVAL_MS 5000
#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1

// registered resources
static const char * const obs_path[] = { "led", NULL };
static const char * const test_path[] = { "testing", NULL };

// TODO add to config files
// #define CONFIG_COAP_RESOURCE "other/block"// "obs"
#define CONFIG_COAP_SERVER_HOSTNAME "83.150.54.152"

static struct coap_client client;
// static struct sockaddr_storage broker;
static struct sockaddr_in broker; // TODO use ipv6 eventually
static int sock;
static int poll_sock;
static u16_t next_token;
#define MESSAGE_ID next_token
static u8_t coap_buf[APP_COAP_MAX_MSG_LEN];

static struct cloud_backend *coap_cloud_backend;

static atomic_t disconnect_requested;
static atomic_t connection_poll_active;

static K_SEM_DEFINE(connection_poll_sem, 0, 1);


static int send_obs_reply_ack(uint16_t id, uint8_t *token, uint8_t tkl)
{
	struct coap_packet request;
	uint8_t *data;
	int r;

	data = (uint8_t *)k_malloc(APP_COAP_MAX_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	r = coap_packet_init(&request, data, APP_COAP_MAX_MSG_LEN,
			     1, COAP_TYPE_ACK, tkl, token, 0, id);
	if (r < 0) {
		LOG_ERR("Failed to init CoAP message");
		goto end;
	}
	r = send(poll_sock, request.data, request.offset, 0);
end:
	k_free(data);

	return r;
}

static int process_simple_coap_reply(void)
{
	struct coap_packet reply;
	uint8_t *data;
	int rcvd;
	int ret;

	data = (uint8_t *)k_malloc(APP_COAP_MAX_MSG_LEN);
	if (!data) {
		LOG_ERR("did not receive data;");
		return -ENOMEM;
	}

  // MSG_DONTWAIT, we dont care if no ack received,
	// maybe has to be handled sometime to save buffer space on the socket?
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

	ret = coap_packet_parse(&reply, data, rcvd, NULL, 0);
	if (ret < 0) {
		LOG_ERR("Invalid data received");
	}

end:
	k_free(data);
	return 0;
}


static int send_simple_coap_request(const struct cloud_msg *const msg)
{
	uint8_t payload[strlen(msg->buf)];
	strcpy(payload, msg->buf);
	struct coap_packet request;
	const char * const *p;
	uint8_t *data;
	int r;

	data = (uint8_t *)k_malloc(APP_COAP_MAX_MSG_LEN);
	if (!data) {
		LOG_ERR("couldn't malloc data");
		return -ENOMEM;
	}

	r = coap_packet_init(&request, data, APP_COAP_MAX_MSG_LEN,
			     1, COAP_TYPE_CON, 8, coap_next_token(),
			     COAP_METHOD_PUT, coap_next_id());
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

	r = coap_packet_append_payload_marker(&request);
	if (r < 0) {
		LOG_ERR("Unable to append payload marker");
		goto end;
	}

  LOG_INF("Appending payload: %s", payload);

	r = coap_packet_append_payload(&request, (uint8_t *)payload,
				       sizeof(payload));
	if (r < 0) {
		LOG_ERR("Not able to append payload");
		goto end;
	}
  LOG_INF("===== SENDING msg in 159");
	r = send(sock, request.data, request.offset, 0);

end:
	k_free(data);

	return 0;
}

static int send_simple_coap_msgs_and_wait_for_reply(const struct cloud_msg *const msg)
{
	int err;
	err = send_simple_coap_request(msg);
	if (err < 0) {
		LOG_INF("got error upon sending: %d", err);
		return err;
	}

	err = process_simple_coap_reply();
	if (err < 0) {
		// TODO cloud error message
		LOG_INF("reply was parsed, err %d", err);
		return err;
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
			     1, COAP_TYPE_CON, 8, coap_next_token(), // TODO changed to non_con from  CON
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
	LOG_INF("===== SENDING msg in 219");
	r = send(poll_sock, request.data, request.offset, 0);

end:
	k_free(data);

	return r;
}

static int process_obs_coap_reply(void)
{

	LOG_INF("processing OBS reply");
	struct coap_packet reply;
	int err;
	uint16_t id;
	uint8_t token[8];
	uint8_t *data;
	uint8_t type;
	uint16_t payload_len;
	const uint8_t *payload;
	uint8_t token_len;
	int rcvd;
	int ret;

	data = (uint8_t *)k_malloc(APP_COAP_MAX_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	LOG_INF("Waiting for LED patch");

	// MSG_DONTWAIT would be non-blocking, but we want blocking
	// as we need the data to update. nrf_recv should support this, but will only
	// work with nrf_socket(), but this did not properly work.
	// recv from zephyr does not support MSG_WAITALL, so we loop until we can read from socket
	// rcvd = nrf_recv(poll_sock, data, APP_COAP_MAX_MSG_LEN, NRF_MSG_WAITALL);
  rcvd = -1;
	// while(rcvd < 0){
	rcvd = recv(poll_sock, data, APP_COAP_MAX_MSG_LEN, 0); //MSG_DONTWAIT); TODO changed this to 0 from MSG_DONTWAIT
	// }

	LOG_INF("got after recv with msg length: %d", rcvd);

	if (rcvd == 0) {
		ret = -EIO;
		LOG_INF("something went wrong, skipping observe answer parsing");
		goto end;
	}

	if (rcvd < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			ret = 0;
		} else {
			ret = -errno;
		}
		LOG_INF("something went wrong, skipping observe answer parsing");
		goto end;
	}

	ret = coap_packet_parse(&reply, data, rcvd, NULL, 0);
	if (ret < 0) {
		LOG_ERR("Invalid data received");
		goto end;
	}

	LOG_INF("also after coap_packet_parse");

	// tkl = coap_header_get_token(&reply, (uint8_t *)token);
	// token_len = coap_header_get_token(&reply, token);
	token_len = coap_header_get_token(&reply, (uint8_t *)token);

	// if ((token_len != sizeof(next_token)) &&
	//     (memcmp(&next_token, token, sizeof(next_token)) != 0)) {
	// 	printk("Invalid token received: 0x%02x%02x\n",
	// 	       token[1], token[0]);
	// 	// return 0;
	// }


	LOG_INF("parsed header_token");
	id = coap_header_get_id(&reply);
	LOG_INF("parsed coap_header_id");
	type = coap_header_get_type(&reply);
	LOG_INF("parsed coap_header_type");



	LOG_INF("ID: %d", id);
	LOG_INF("TYpe: %d", type);
	LOG_INF("token: %s", token);

	// send back ACK if received type con
	if (type == COAP_TYPE_ACK) {
		LOG_INF("received ACK");
		ret = 0;
		// in case of udpate, confirm update received from server
	} else if (type == COAP_TYPE_CON) {
		LOG_INF("recieved CON");
		ret = send_obs_reply_ack(id, token, token_len);
	} else {
		// probably a RST, e.g. type 3
		LOG_INF("ERROR, received RST");
		ret = 0;
	  // return ret;
	}



	payload = coap_packet_get_payload(&reply, &payload_len);



	LOG_INF("GOT payload:  %s", payload);

	// err = json_escape(payload, sizeof(payload), sizeof(payload));
  // if(err < 0){
	// 	LOG_INF("could not parse json");
	// }
	// LOG_INF("parsed payload: %s", payload);

	// TODO replace test_data with payload.

	struct cloud_event cloud_evt = {
		.type = CLOUD_EVT_DATA_RECEIVED,
		.data.msg.buf = payload, //&payload,
		.data.msg.len = payload_len //payload
	};
	cloud_notify_event(coap_cloud_backend, &cloud_evt, payload);

end:
	k_free(data);

	return ret;
}

/* stop observing */
static int send_obs_reset_coap_request(void)
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
			     1, COAP_TYPE_CON, 8, coap_next_token(), // TODO changing from COAP_TYPE_CON to non confirmable
			     0, coap_next_id());
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

	r = send(poll_sock, request.data, request.offset, 0);

end:
	k_free(data);

	return r;
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

    // this would probably not be necessary, but ensures the endpoint is available
		// before the application starts sending any data
		err = connect(sock, (struct sockaddr *)&broker, sizeof(struct sockaddr_in));
	  if (err < 0) {
	    LOG_ERR("Connect failed : %d", errno);
	    return -errno;
	  }

  LOG_INF("Created broker sock: %d", sock);

  // init_coap_client
	client.sock = sock;
	client.broker = &broker;


	// create second socket for polling thread
	// poll_sock = nrf_socket(NRF_AF_INET, NRF_SOCK_DGRAM, NRF_IPPROTO_UDP);
	poll_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	 if (poll_sock < 0) {
	   LOG_ERR("Failed to create CoAP socket: %d.", errno);
	   return -errno;
	 }

	 err = connect(poll_sock, (struct sockaddr *)&broker, sizeof(struct sockaddr_in));
	 if (err < 0) {
	   LOG_ERR("Connect for poll sock failed : %d", errno);
	   return -errno;
	 }
	 LOG_INF("Created observe poll sock: %d", poll_sock);

	// should send connected and ready events
	LOG_INF("Cloud backend ready, triggering CONNECTED and READY");
	struct cloud_backend_config *config = coap_cloud_backend->config;
	struct cloud_event cloud_evt_connected = { CLOUD_EVT_CONNECTED };
	struct cloud_event cloud_evt_ready = { CLOUD_EVT_READY };
	cloud_notify_event(coap_cloud_backend, &cloud_evt_connected, config->user_data);
  cloud_notify_event(coap_cloud_backend, &cloud_evt_ready, config->user_data);

	return err;
}

#ifdef CONFIG_BOARD_QEMU_X86
#define POLL_THREAD_STACK_SIZE 4096
#else
#define POLL_THREAD_STACK_SIZE 16384
#endif

static void observe(void){

	LOG_INF("Started observation thread");

	int r;

	/* wait for the thread semaphore being freed by the connection poll start */
	k_sem_take(&connection_poll_sem, K_FOREVER);

	LOG_INF("Was able to start the polling");

	/* then start observing */
	r = send_obs_coap_request();
	if (r < 0) {
		// todo cloud error event
		return r;
	}

	while (1) {
		if(atomic_get(&disconnect_requested)){
			LOG_INF("expected disconnect event");
			return;
		}

		r = process_obs_coap_reply();
		LOG_INF("RESULT OF process_obs_coap_reply: %d", r);
		if (r < 0) {
			// todo cloud error event
			return r;
		}

		/* Unregister if stopped, and close socket afterwards */
		// if (!atomic_get(&connection_poll_active)) {
		// 	r = send_obs_reset_coap_request();
		// 	if(r < 0){
		// 		LOG_ERR("Failed to reset observe. Stopping listener anyway.");
		// 	}
		// 	k_sem_give(&connection_poll_sem);
		// 	(void)nrf_close(poll_sock);
		// }
	}
	return 0;
}

/* defines the observe thread to get updates from resource server */
K_THREAD_DEFINE(connection_poll_thread, POLL_THREAD_STACK_SIZE,
		observe, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static int connection_poll_start(void)
{
	/* give semaphore to allow observe thread to start listening on poll sock */
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
	LOG_DBG("keepalive_time_left called, not expected.");
	return 5;
}

int coap_cloud_input(void)
{
	LOG_DBG("received data in cloud_input, not expected.");
	return 0;
}

int coap_cloud_connect(void)
{
	int err;
		atomic_set(&disconnect_requested, 0);
		err = broker_init();

		if (err) {
			LOG_ERR("client_broker_init, error: %d", err);
			return err;
		}
		err = connect_error_translate(err);
		err = connection_poll_start();
	return err;
}

static int c_init(const struct cloud_backend *const backend,
		  cloud_evt_handler_t handler)
{
	__ASSERT(CONFIG_CLOUD_API, "Fatal error: CLOUD_API required. Please set CLOUD to true in options.");

	backend->config->handler = handler;
	coap_cloud_backend = (struct cloud_backend *)backend;

	struct coap_cloud_config config = {
		.client_id = backend->config->id,
		.client_id_len = backend->config->id_len
	};
  return 0;
}

static int c_ep_subscriptions_add(const struct cloud_backend *const backend,
				  const struct cloud_endpoint *const list,
				  size_t list_count)
{
	return 0;
}

static int c_connect(const struct cloud_backend *const backend)
{
	return coap_cloud_connect();
}

static int c_disconnect(const struct cloud_backend *const backend)
{
	atomic_set(&disconnect_requested, 1);
	(void)nrf_close(sock);
	return 0;
}

static int c_send(const struct cloud_backend *const backend,
		  const struct cloud_msg *const msg)
{
	// LOG_INF("SENDING SENSOR DATA. MESSAGE: %s\n", msg->buf);
	// TODO temporary disabled to test observe
	int err;
	err = send_simple_coap_msgs_and_wait_for_reply(msg);
	return 0; //err;
}

/* called periodically to keep connection to broker alive
 * @return 0 if successful, error code otherwies */
static int c_ping()
{
	LOG_DBG("c_ping called, this was not expected.");
	return 0;
}

static int c_keepalive_time_left(const struct cloud_backend *const backend)
{
	return 5;
}

static const struct cloud_api coap_cloud_api = {
	.init			    = c_init,
	.connect		  = c_connect,
	.disconnect		= c_disconnect,
	.send		    	= c_send,
	.ping		    	= c_ping,
	.keepalive_time_left	= c_keepalive_time_left,
	.input			  = coap_cloud_input,
	.ep_subscriptions_add	= c_ep_subscriptions_add
};


CLOUD_BACKEND_DEFINE(COAP_CLOUD, coap_cloud_api);
