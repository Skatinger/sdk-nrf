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
#include "cJSON.h"
#include "cJSON_os.h"
// #include <lib/cbor.h>
#include "tinycbor/cbor.h"
// #include <tinycbor/cbor_mbuf_writer.h> // THIS IS USED ONLY in the example code
// #include <tinycbor/cbor_mbuf_reader.h> // naming went to shit ...
#include <tinycbor/cbor_buf_writer.h>
#include <tinycbor/cbor_buf_reader.h>



LOG_MODULE_REGISTER(coap_cloud, CONFIG_COAP_CLOUD_LOG_LEVEL);

// COAP stuff
#define APP_COAP_SEND_INTERVAL_MS 5000
#define APP_COAP_MAX_MSG_LEN 1280
#define APP_COAP_VERSION 1

// registered resources
static const char * const obs_path[] = { "led", NULL };
static const char * const test_path[] = { "testing", NULL };

// TODO add to config files
#define CONFIG_COAP_SERVER_HOSTNAME "83.150.54.152"

static struct coap_client client;
static struct sockaddr_in broker;
static int sock;
static int poll_sock;
static u16_t next_token;
#define MESSAGE_ID next_token
static u8_t coap_buf[APP_COAP_MAX_MSG_LEN];

static struct cloud_backend *coap_cloud_backend;
// allow to stop the polling
static atomic_t disconnect_requested;
static K_SEM_DEFINE(connection_poll_sem, 0, 1);

/* send a reply if an observation has been received
 * takes the message id, token and the token length of the CON message
 * for which this acc should be sent */
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

/* processes a simple coap request, skips if nothing received,
 * as we do not need confirmation from server, we send data even if not reachable,
 * in case the server comes online again */
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

/* used to send a simple coap PUT request to the broker (server) */
static int send_simple_coap_request(const struct cloud_msg *const msg)
{
	uint8_t payload[strlen(msg->buf)];
	strcpy(payload, msg->buf);
	struct coap_packet request;
	const char * const *p;
	uint8_t *data;
	int r;
	char *path = "";

  /* parse json */
	const cJSON *appId = NULL;
	cJSON *json_packet =cJSON_Parse(msg->buf);
	if (json_packet == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
  	if (error_ptr != NULL) {
		  LOG_ERR("Error in json parsing: %s", error_ptr);
    }
		LOG_INF("json packet not found");
		cJSON_Delete(json_packet);
		return 0;
  }

	appId = cJSON_GetObjectItemCaseSensitive(json_packet, "appId");
	if (!cJSON_IsString(appId) || (appId->valuestring == NULL)) {
		LOG_INF("can not use this payload: %s", payload);
		cJSON_Delete(json_packet);
		return 0;
	}
	// parse data points
	cJSON *data_value = cJSON_GetObjectItemCaseSensitive(json_packet, "data");
  float value_to_send;

  // only send if humid, temp or air_press
	if(!strcmp("HUMID", appId->valuestring)){
    path = "humid";
  } else if(!strcmp("TEMP", appId->valuestring)){
    path = "temp";
	} else if(!strcmp("AIR_PRESS", appId->valuestring)){
    path = "air_press";
	// } else if(!strcmp("BUTTON", appId->valuestring)){
  //   path = "button";
	} else {
		// dont need this data, skip
		cJSON_Delete(json_packet);
		return 0;
	}

	value_to_send = atof(data_value->valuestring);
  cJSON_Delete(json_packet);

  /* encoding payload to cbor */
	int err;
	struct cbor_buf_writer buf_writer;
	struct cbor_buf_reader buf_reader;

	uint8_t encodedPayload[16];

  CborParser parser;
	CborValue value;
	int result;
	CborEncoder encoder;

	cbor_buf_writer_init(&buf_writer, encodedPayload, sizeof(encodedPayload));
	cbor_encoder_init(&encoder, &buf_writer.enc, 0);
	if(cbor_encode_float(&encoder, value_to_send) != CborNoError){
		LOG_ERR("error encoding int");
	}

  /* build coap message */
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

	r = coap_packet_append_option(&request, COAP_OPTION_URI_PATH,
					      path, strlen(path));
		if (r < 0) {
			LOG_ERR("Unable add option to request");
			goto end;
		}

	r = coap_packet_append_payload_marker(&request);
	if (r < 0) {
		LOG_ERR("Unable to append payload marker");
		goto end;
	}

  LOG_INF("Appending payload: %s", payload);
  // changing to payload buffer which is encoded
	r = coap_packet_append_payload(&request, (uint8_t *)encodedPayload,
				       sizeof(encodedPayload));
	if (r < 0) {
		LOG_ERR("Not able to append payload");
		goto end;
	}

	r = send(sock, request.data, request.offset, 0);

end:
	k_free(data);

	return 0;
}

/* helper to send data and recv the response, if any */
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
		LOG_INF("reply was parsed, err %d", err);
		return err;
	}
	return 0;
}

/* sends observe requests to the broker, to the configured obs_paths
 * currently obs paths are just /led, but could be extended in head of this file */
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
	r = send(poll_sock, request.data, request.offset, 0);

end:
	k_free(data);

	return r;
}

/* processes messages on the poll_sock, reserved for observe events
 * responds with ACKs if necessary and parses payload. Will create a cloud_notify_event
 * if any payload was received
 * This function is periodcally called by the observe() thread */
static int process_obs_coap_reply(void)
{
	struct coap_packet reply;
	int err;
	uint16_t id;
	uint8_t token[8];
	uint8_t *data;
	uint8_t type;
	uint16_t payload_len;
	uint8_t *payload;
	uint8_t token_len;
	int rcvd;
	int ret;

	data = (uint8_t *)k_malloc(APP_COAP_MAX_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	LOG_INF("Waiting for LED patch");
  // recv blocks by default, will only act if a patch comes in
	rcvd = recv(poll_sock, data, APP_COAP_MAX_MSG_LEN, 0);

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

  // parse message headers
	token_len = coap_header_get_token(&reply, (uint8_t *)token);
	id = coap_header_get_id(&reply);
	type = coap_header_get_type(&reply);

	// do nothing if ACK received, send back ACK if received type CON
	if (type == COAP_TYPE_ACK) {
		ret = 0;
		// in case of udpate, confirm update received from server
	} else if (type == COAP_TYPE_CON) {
		ret = send_obs_reply_ack(id, token, token_len);
	} else {
		// should never get here, this might only happen if server ran into problems
		// with our request and returns RST or invalid message
		LOG_ERR("ERROR, unexpected message received.");
		return -1;
	}

	payload = coap_packet_get_payload(&reply, &payload_len);

	// now parse payload
	struct cbor_buf_reader buf_reader;
  CborParser parser;
	CborValue value;
	int result;
	cbor_buf_reader_init(&buf_reader,payload, payload_len);
	if (cbor_parser_init(&buf_reader.r, 0, &parser, &value) != CborNoError){
		LOG_INF("failed initialzing parser");
	}
	if(cbor_value_get_int(&value, &result) != CborNoError){
		LOG_INF("error getting integer");
	}

  // convert integer to hex, for a color
	// pad with 0's
	char hex[20];
	sprintf(hex, "%06x", result);

 // create json for cloud update
 char message[APP_COAP_MAX_MSG_LEN];
 strcpy(message, "{\"appId\":\"LED\",\"data\":{\"color\":\"");
 strcat(message, hex);
 strcat(message, "\"},\"messageType\":\"CFG_SET\"}");

 LOG_INF("Message for cloud: %s", message);

  // trigger cloud event with received data
	struct cloud_event cloud_evt = {
		.type = CLOUD_EVT_DATA_RECEIVED,
		.data.msg.buf = message,
		.data.msg.len = sizeof(message)
	};
	cloud_notify_event(coap_cloud_backend, &cloud_evt, message);

end:
	k_free(data);

	return ret;
}

/* resets the observation to stop observing */
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
			     1, COAP_TYPE_CON, 8, coap_next_token(),
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

/* initialize endpoints (broker) for the communication, setup sockets and connections */
static int broker_init(void) {

	int err;

	char *ip_address = CONFIG_COAP_SERVER_HOSTNAME; // "83.150.54.152";
	broker = parse_ip_address(5683, ip_address); // use standard coap port

	// initialize socket for coap message sending on UDP
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

  LOG_DBG("Created broker socket: %d", sock);

  // init_coap_client
	client.sock = sock;
	client.broker = &broker;


	// create second socket for polling thread
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
	 LOG_DBG("Created observe poll socket: %d", poll_sock);

	// should send connected and ready events
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

/* runs in the separate observe thread
 * starts an observation request and then keeps calling "process_obs_coap_reply"
 * to parse observation updates from the server */
static void observe(void){
	LOG_DBG("Started observation thread");

	int r;

	/* wait for the thread semaphore being freed by the connection poll start
	 * this will be done by the main application thread once the setup is complete */
	k_sem_take(&connection_poll_sem, K_FOREVER);

	/* then start observing by sending an observation request */
	r = send_obs_coap_request();
	if (r < 0) {
		return r;
	}

  // keep parsing inputs until application stops
	while (1) {
		// if a disconnect was requested by the application
		if(atomic_get(&disconnect_requested)){
			LOG_INF("expected disconnect event");
			r = send_obs_reset_coap_request();
				if(r < 0){
					LOG_ERR("Failed to reset observe. Stopping listener anyway.");
				}
				k_sem_give(&connection_poll_sem);
				(void)nrf_close(poll_sock);
			return 0;
		}

		r = process_obs_coap_reply();
		LOG_INF("RESULT OF process_obs_coap_reply: %d", r);
		if (r < 0) {
			return r;
		}
	}
	return 0;
}

/* defines the observe thread to get updates from resource server,
 * starts directly upon application start, is blocked by a semaphore until
 * until main application thread is ready with the setup */
K_THREAD_DEFINE(connection_poll_thread, POLL_THREAD_STACK_SIZE,
		observe, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/* starts the observe thread by giving the connection_poll_sem semaphore to
 * the observation thread  */
static int connection_poll_start(void)
{
	/* give semaphore to allow observe thread to start listening on poll sock */
	atomic_set(&disconnect_requested, 0);
	k_sem_give(&connection_poll_sem);
	return 0;
}

/* not implemented, but required for the could api */
int coap_cloud_keepalive_time_left(void)
{
	LOG_DBG("keepalive_time_left called, not implemented.");
	return 5;
}

/* not implemented, but required for the could api */
int coap_cloud_input(void)
{
	LOG_DBG("received data in cloud_input, not implemented.");
	return 0;
}

/* initializes the cloud connection */
int coap_cloud_connect(void)
{
	int err;
		atomic_set(&disconnect_requested, 0);
		err = broker_init();

		if (err) {
			LOG_ERR("client_broker_init, error: %d", err);
			return err;
		}
		// err = connect_error_translate(err);
		err = connection_poll_start();
	return err;
}

/* initializes the cloud backend */
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
	int err;
	err = send_simple_coap_msgs_and_wait_for_reply(msg);
	return err;
}

static const struct cloud_api coap_cloud_api = {
	.init			    = c_init,
	.connect		  = c_connect,
	.disconnect		= c_disconnect,
	.send		    	= c_send,
	.input			  = coap_cloud_input
};


CLOUD_BACKEND_DEFINE(COAP_CLOUD, coap_cloud_api);
