/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-openssl.c: Asyncronous Callback-based SOAP Request Queue.
 *
 * Authors:
 *      Alex Graveley (alex@ximian.com)
 *
 * Copyright (C) 2001, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_OPENSSL_SSL_H

#include <glib.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "soup-openssl.h"

typedef struct {
	GIOChannel   channel;
	gint         fd;
	GIOChannel  *real_sock;
	SSL         *ssl;
} SoupOpenSSLChannel;

static GIOError
soup_openssl_read (GIOChannel   *channel,
		   gchar        *buf,
		   guint         count,
		   guint        *bytes_read)
{
	SoupOpenSSLChannel *chan = (SoupOpenSSLChannel *) channel;
	gint result;

	result = SSL_read (chan->ssl, buf, count);

	if (result < 0) {
		*bytes_read = 0;
		switch (errno) {
		case EINVAL:
			return G_IO_ERROR_INVAL;
		case EAGAIN:
		case EINTR:
			return G_IO_ERROR_AGAIN;
		default:
			return G_IO_ERROR_UNKNOWN;
		}
	} else {
		*bytes_read = result;
		return G_IO_ERROR_NONE;
	}
}

static GIOError
soup_openssl_write (GIOChannel   *channel,
		    gchar        *buf,
		    guint         count,
		    guint        *bytes_written)
{
	SoupOpenSSLChannel *chan = (SoupOpenSSLChannel *) channel;
	gint result;

	result = SSL_write (chan->ssl, buf, count);

	if (result < 0) {
		*bytes_written = 0;
		switch (errno) {
		case EINVAL:
			return G_IO_ERROR_INVAL;
		case EAGAIN:
		case EINTR:
			return G_IO_ERROR_AGAIN;
		default:
			return G_IO_ERROR_UNKNOWN;
		}
	} else {
		*bytes_written = result;
		return G_IO_ERROR_NONE;
	}
}

static GIOError
soup_openssl_seek (GIOChannel *channel, gint offset, GSeekType type)
{
	SoupOpenSSLChannel *chan = (SoupOpenSSLChannel *) channel;
	return g_io_channel_seek (chan->real_sock, offset, type);
}

static void
soup_openssl_close (GIOChannel   *channel)
{
	SoupOpenSSLChannel *chan = (SoupOpenSSLChannel *) channel;
	g_io_channel_close (chan->real_sock);
}

static void
soup_openssl_free (GIOChannel   *channel)
{
	SoupOpenSSLChannel *chan = (SoupOpenSSLChannel *) channel;
	g_io_channel_unref (chan->real_sock);
	SSL_free (chan->ssl);
	g_free (chan);
}

#if 0

/* Commented out until we can figure out why SSL_pending always fails */

typedef struct {
	GIOFunc         func;
	gpointer        user_data;
} SoupOpenSSLReadData;

static gboolean 
soup_openssl_read_cb (GIOChannel   *channel, 
		      GIOCondition  condition, 
		      gpointer      user_data)
{
	SoupOpenSSLChannel *chan = (SoupOpenSSLChannel *) channel;
	SoupOpenSSLReadData *data = user_data;

	if (condition & G_IO_IN) {
		if (SSL_pending (chan->ssl) && 
		    !(*data->func) (channel, condition, data->user_data)) {
			g_free (data);
			return FALSE;
		}
		return TRUE;
	} else return (*data->func) (channel, condition, data->user_data);
}

static guint
soup_openssl_add_watch (GIOChannel     *channel,
			gint            priority,
			GIOCondition    condition,
			GIOFunc         func,
			gpointer        user_data,
			GDestroyNotify  notify)
{
	SoupOpenSSLChannel *chan = (SoupOpenSSLChannel *) channel;

	if (condition & G_IO_IN) {
		SoupOpenSSLReadData *data = g_new0 (SoupOpenSSLReadData, 1);
		data->func = func;
		data->user_data = user_data;

		return chan->real_sock->funcs->io_add_watch (channel, 
							     priority, 
							     condition,
							     soup_openssl_read_cb,
							     data,
							     notify);
	} else return chan->real_sock->funcs->io_add_watch (channel, 
							    priority, 
							    condition,
							    func,
							    user_data,
							    notify);
}

#endif /* 0 */

static guint
soup_openssl_add_watch (GIOChannel     *channel,
			gint            priority,
			GIOCondition    condition,
			GIOFunc         func,
			gpointer        user_data,
			GDestroyNotify  notify)
{
	SoupOpenSSLChannel *chan = (SoupOpenSSLChannel *) channel;
	return chan->real_sock->funcs->io_add_watch (channel, 
						     priority, 
						     condition,
						     func,
						     user_data,
						     notify);
}

GIOFuncs soup_openssl_channel_funcs = {
	soup_openssl_read,
	soup_openssl_write,
	soup_openssl_seek,
	soup_openssl_close,
	soup_openssl_add_watch,
	soup_openssl_free,
};

static SSL_CTX *ssl_context = NULL;

GIOChannel *
soup_openssl_get_iochannel (GIOChannel *sock)
{
	SoupOpenSSLChannel *chan;
	GIOChannel *gchan;
	int bits, alg_bits, err, sockfd;
	SSL *ssl;
	X509 *cert;

        g_return_val_if_fail (sock != NULL, NULL);

	if (!ssl_context && !soup_openssl_init ()) goto THROW_CREATE_ERROR;
	
	sockfd = g_io_channel_unix_get_fd (sock);
	if (!sockfd) goto THROW_CREATE_ERROR;

	ssl = SSL_new (ssl_context);
	if (!ssl) {
		g_warning ("SSL object creation failure.");
		goto THROW_CREATE_ERROR;
	}

	err = SSL_set_fd (ssl, sockfd);
	if (err == 0) {
		g_warning ("Unable to set SSL file descriptor.");
		goto THROW_CREATE_ERROR;
	}

	SSL_connect (ssl);
	if (err == 0) {
		g_warning ("Secure connection could not be established.");
		goto THROW_CREATE_ERROR;
	}

	bits = SSL_get_cipher_bits (ssl, &alg_bits);
	if (bits == 0) {
		g_warning ("Server requested unsecure tranfer."); 
		goto THROW_CREATE_ERROR;
	}

	cert = SSL_get_peer_certificate (ssl);
	if (!cert) {
		g_warning ("Server certificate unavailable");
		goto THROW_CREATE_ERROR;
	}
	X509_free (cert);

	chan = g_new0 (SoupOpenSSLChannel, 1);
	chan->fd = sockfd;
	chan->real_sock = sock;
	chan->ssl = ssl;
	g_io_channel_ref (sock);

	gchan = (GIOChannel *) chan;
	gchan->funcs = &soup_openssl_channel_funcs;
	g_io_channel_init (gchan);

	return gchan;

 THROW_CREATE_ERROR:
	return NULL;
}

gboolean
soup_openssl_init (void)
{
	SSL_library_init ();
	SSL_load_error_strings ();

	ssl_context = SSL_CTX_new (SSLv23_client_method ());
	if (!ssl_context) {
		g_warning ("Unable to initialize OpenSSL library");
		return FALSE;
	}
	
	SSL_CTX_set_default_verify_paths (ssl_context);

	return TRUE;
}

void 
soup_openssl_set_security_policy (SoupSecurityPolicy policy)
{
	switch (policy) {
	case SOUP_SECURITY_DOMESTIC:
		break;
	case SOUP_SECURITY_EXPORT:
		break;
	case SOUP_SECURITY_FRANCE:
		break;
	}
}

#endif /*HAVE_OPENSSL_SSL_H*/
