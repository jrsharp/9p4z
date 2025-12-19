/*
 * Copyright (c) 2025 9p4z Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/9p/session_pool.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(ninep_session_pool, CONFIG_NINEP_LOG_LEVEL);

int ninep_session_pool_init(struct ninep_session_pool *pool,
                              const struct ninep_session_pool_config *config)
{
	int ret;

	if (!pool || !config || !config->fs_ops || config->max_sessions <= 0) {
		return -EINVAL;
	}

	memset(pool, 0, ninep_session_pool_size(config->max_sessions));

	pool->max_sessions = config->max_sessions;
	pool->fs_ops = config->fs_ops;
	pool->fs_context = config->fs_context;
	pool->auth_config = config->auth_config;

	ret = k_mutex_init(&pool->lock);
	if (ret < 0) {
		LOG_ERR("Failed to initialize pool mutex: %d", ret);
		return ret;
	}

	/* Initialize all sessions as free */
	for (int i = 0; i < pool->max_sessions; i++) {
		pool->sessions[i].state = NINEP_SESSION_FREE;
		pool->sessions[i].session_id = i;
		pool->sessions[i].transport_priv = NULL;
	}

	LOG_INF("Session pool initialized: %d sessions", pool->max_sessions);
	return 0;
}

struct ninep_session *ninep_session_alloc(struct ninep_session_pool *pool)
{
	struct ninep_session *session = NULL;

	if (!pool) {
		return NULL;
	}

	k_mutex_lock(&pool->lock, K_FOREVER);

	/* Find first free session */
	for (int i = 0; i < pool->max_sessions; i++) {
		if (pool->sessions[i].state == NINEP_SESSION_FREE) {
			session = &pool->sessions[i];
			session->state = NINEP_SESSION_ALLOCATED;
			LOG_INF("Allocated session %d", session->session_id);
			break;
		}
	}

	k_mutex_unlock(&pool->lock);

	if (!session) {
		LOG_WRN("Session pool exhausted (%d/%d in use)",
		        pool->max_sessions, pool->max_sessions);
	}

	return session;
}

void ninep_session_connected(struct ninep_session *session)
{
	if (!session) {
		return;
	}

	session->state = NINEP_SESSION_CONNECTED;
	LOG_INF("Session %d connected", session->session_id);
}

void ninep_session_free(struct ninep_session *session)
{
	if (!session) {
		return;
	}

	LOG_INF("Freeing session %d", session->session_id);

	session->state = NINEP_SESSION_DISCONNECTING;

	/* Stop transport if it has a stop function */
	if (session->transport.ops && session->transport.ops->stop) {
		session->transport.ops->stop(&session->transport);
	}

	/* Clean up server state */
	/* Note: ninep_server doesn't currently have a cleanup function,
	 * but we reset state here for future use */
	memset(&session->server, 0, sizeof(session->server));
	memset(&session->transport, 0, sizeof(session->transport));

	session->transport_priv = NULL;
	session->state = NINEP_SESSION_FREE;

	LOG_INF("Session %d freed", session->session_id);
}

struct ninep_session *ninep_session_get(struct ninep_session_pool *pool, int session_id)
{
	if (!pool || session_id < 0 || session_id >= pool->max_sessions) {
		return NULL;
	}

	return &pool->sessions[session_id];
}

void ninep_session_pool_disconnect_all(struct ninep_session_pool *pool)
{
	if (!pool) {
		return;
	}

	LOG_INF("Disconnecting all sessions");

	k_mutex_lock(&pool->lock, K_FOREVER);

	for (int i = 0; i < pool->max_sessions; i++) {
		if (pool->sessions[i].state != NINEP_SESSION_FREE) {
			ninep_session_free(&pool->sessions[i]);
		}
	}

	k_mutex_unlock(&pool->lock);
}
