/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

/**@file
 *@brief AWS IoT library header.
 */

#ifndef COAP_CLOUD_H__
#define COAP_CLOUD_H__

#include <stdio.h>
#include <net/mqtt.h>

/**
 * @defgroup coap_cloud AWS IoT library
 * @{
 * @brief Library to connect the device to the AWS IoT message broker.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief AWS IoT shadow topics, used in messages to specify which shadow
 *         topic that will be published to.
 */
enum coap_cloud_topic_type {
    COAP_CLOUD_SHADOW_TOPIC_UNKNOWN = 0x0,
    COAP_CLOUD_SHADOW_TOPIC_GET,
    COAP_CLOUD_SHADOW_TOPIC_UPDATE,
    COAP_CLOUD_SHADOW_TOPIC_DELETE
};

/**@ AWS broker disconnect results. */
enum aws_disconnect_result {
    COAP_CLOUD_DISCONNECT_USER_REQUEST,
    COAP_CLOUD_DISCONNECT_CLOSED_BY_REMOTE,
    COAP_CLOUD_DISCONNECT_INVALID_REQUEST,
    COAP_CLOUD_DISCONNECT_MISC,
    COAP_CLOUD_DISCONNECT_COUNT
};

/**@brief AWS broker connect results. */
enum aws_connect_result {
    COAP_CLOUD_CONNECT_RES_SUCCESS = 0,
    COAP_CLOUD_CONNECT_RES_ERR_NOT_INITD = -1,
    COAP_CLOUD_CONNECT_RES_ERR_INVALID_PARAM = -2,
    COAP_CLOUD_CONNECT_RES_ERR_NETWORK = -3,
    COAP_CLOUD_CONNECT_RES_ERR_BACKEND = -4,
    COAP_CLOUD_CONNECT_RES_ERR_MISC = -5,
    COAP_CLOUD_CONNECT_RES_ERR_NO_MEM = -6,
    /* Invalid private key */
    COAP_CLOUD_CONNECT_RES_ERR_PRV_KEY = -7,
    /* Invalid CA or client cert */
    COAP_CLOUD_CONNECT_RES_ERR_CERT = -8,
    /* Other cert issue */
    COAP_CLOUD_CONNECT_RES_ERR_CERT_MISC = -9,
    /* Timeout, SIM card may be out of data */
    COAP_CLOUD_CONNECT_RES_ERR_TIMEOUT_NO_DATA = -10,
    COAP_CLOUD_CONNECT_RES_ERR_ALREADY_CONNECTED = -11,
};

/** @brief AWS IoT notification event types, used to signal the application. */
// TODO are these automatically correct?
enum coap_cloud_evt_type {
    /** Connecting to AWS IoT broker. */
    COAP_CLOUD_EVT_CONNECTING = 0x1,
    /** Connected to AWS IoT broker. */
    COAP_CLOUD_EVT_CONNECTED,
    /** AWS IoT broker ready. */
    COAP_CLOUD_EVT_READY,
    /** Disconnected to AWS IoT broker. */
    COAP_CLOUD_EVT_DISCONNECTED,
    /** Data received from AWS message broker. */
    COAP_CLOUD_EVT_DATA_RECEIVED,
    /** FOTA update start. */
    COAP_CLOUD_EVT_FOTA_START,
    /** FOTA update done, request to reboot. */
    COAP_CLOUD_EVT_FOTA_DONE,
    /** FOTA erase pending. */
    COAP_CLOUD_EVT_FOTA_ERASE_PENDING,
    /** FOTA erase done. */
    COAP_CLOUD_EVT_FOTA_ERASE_DONE,
    /** AWS IoT library error. */
    COAP_CLOUD_EVT_ERROR
};

/** @brief AWS IoT topic data. */
struct coap_cloud_topic_data {
    /** Type of shadow topic that will be published to. */
    enum coap_cloud_topic_type type;
    /** Pointer to string of application specific topic. */
    const char *str;
    /** Length of application specific topic. */
    size_t len;
};

/** @brief Structure used to declare a list of application specific topics
 *         passed in by the application.
 */
// struct coap_cloud_app_topic_data {
//     /** List of application specific topics. */
//      // struct coap_topic list[5];
//     /** Number of entries in topic list. */
//     size_t list_count;
// };


/** @brief struct to hold data related to the coap client */
struct coap_client {
  // socket for communication
  int sock;
  struct sockaddr_in *broker;

};

/** @brief AWS IoT transmission data. */
struct coap_cloud_data {
    /** Topic data is sent/received on. */
    struct coap_cloud_topic_data topic;
    /** Pointer to data sent/received from the AWS IoT broker. */
    char *ptr;
    /** Length of data. */
    size_t len;
    /** Quality of Service of the message. */
    enum mqtt_qos qos;
};

/** @brief Struct with data received from AWS IoT broker. */
struct coap_cloud_evt {
    /** Type of event. */
    enum coap_cloud_evt_type type;
    union {
        struct coap_cloud_data msg;
        int err;
        bool persistent_session;
    } data;
};

/** @brief AWS IoT library asynchronous event handler.
 *
 *  @param[in] evt The event and any associated parameters.
 */
typedef void (*coap_cloud_evt_handler_t)(const struct coap_cloud_evt *evt);

/** @brief Structure for AWS IoT broker connection parameters. */
struct coap_cloud_config {
    /** Socket for AWS IoT broker connection */
    int socket;
    /** Client id for AWS IoT broker connection, used when
     *  CONFIG_coap_cloud_CLIENT_ID_APP is set. If not set an internal
     *  configurable static client id is used.
     */
    char *client_id;
    /** Length of client_id string. */
    size_t client_id_len;
};

/** @brief Initialize the module.
 *
 *  @warning This API must be called exactly once, and it must return
 *           successfully.
 *
 *  @param[in] config Pointer to struct containing connection parameters.
 *  @param[in] event_handler Pointer to event handler to receive AWS IoT module
 *                           events.
 *
 *  @return 0 If successful.
 *            Otherwise, a (negative) error code is returned.
 */
int coap_cloud_init(const struct coap_cloud_config *const config,
                    coap_cloud_evt_handler_t event_handler);

/** @brief Connect to the AWS IoT broker.
 *
 *  @details This function exposes the MQTT socket to main so that it can be
 *           polled on.
 *
 *  @param[out] config Pointer to struct containing connection parameters,
 *                     the MQTT connection socket number will be copied to the
 *                     socket entry of the struct.
 *
 *  @return 0 If successful.
 *            Otherwise, a (negative) error code is returned.
 */
int coap_cloud_connect(struct coap_cloud_config *const config);

/** @brief Disconnect from the AWS IoT broker.
 *
 *  @return 0 If successful.
 *            Otherwise, a (negative) error code is returned.
 */
int coap_cloud_disconnect(void);

/** @brief Send data to AWS IoT broker.
 *
 *  @param[in] tx_data Pointer to struct containing data to be transmitted to
 *                     the AWS IoT broker.
 *
 *  @return 0 If successful.
 *            Otherwise, a (negative) error code is returned.
 */
int coap_cloud_send(char* string);//const struct *const coap_cloud_data); // TODO add data *const tx_data);

/** @brief Get data from AWS IoT broker
 *
 *  @return 0 If successful.
 *            Otherwise, a (negative) error code is returned.
 */
int coap_cloud_input(void);

/** @brief Ping AWS IoT broker. Must be called periodically
 *         to keep connection to broker alive.
 *
 *  @return 0 If successful.
 *            Otherwise, a (negative) error code is returned.
 */
int coap_cloud_ping(void);

/** @brief Add a list of application specific topics that will be subscribed to
 *         upon connection to AWS IoT broker.
 *
 *  @param[in] topic_list Pointer to list of topics.
 *  @param[in] list_count Number of entries in the list.
 *
 *  @return 0 If successful.
 *            Otherwise, a (negative) error code is returned.
 */
int coap_cloud_subscription_topics_add(
        const struct coap_cloud_topic_data *const topic_list,
        size_t list_count);

#ifdef __cplusplus
}
#endif

/**
 *@}
 */

#endif /* COAP_CLOUD_H__ */
