/*
 * ws protocol handler plugin for "lws-minimal"
 *
 * Written in 2010-2019 by Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 * This version holds a single message at a time, which may be lost if a new
 * message comes.  See the minimal-ws-server-ring sample for the same thing
 * but using an lws_ring ringbuffer to hold up to 8 messages at a time.
 */

#if !defined (LWS_PLUGIN_STATIC)
#define LWS_DLL
#define LWS_INTERNAL
#include <libwebsockets.h>
#endif

#include <string.h>
#include <stdio.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <pthread.h>
#include <net/if.h>
#include <inttypes.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

/* one of these created for each message */

struct msg {
	void *payload; /* is malloc'd */
	size_t len;
};

/* one of these is created for each client connecting to us */

struct per_session_data__minimal {
	struct per_session_data__minimal *pss_list;
	struct lws *wsi;
	uint32_t tail;
};

/* one of these is created for each vhost our protocol is used with */

struct per_vhost_data__minimal {
	struct lws_context *context;
	struct lws_vhost *vhost;
	const struct lws_protocols *protocol;

	struct per_session_data__minimal *pss_list; /* linked-list of live pss*/
	pthread_t pthread_spam[2];

	pthread_mutex_t lock_ring; /* serialize access to the ring buffer */
	struct lws_ring *ring; /* {lock_ring} ringbuffer holding unsent content */

	const char *config;
	char finished;
};

int canFD;


/* destroys the message when everyone has had a copy of it */

static void __minimal_destroy_message(void *_msg)
{
	struct msg *msg = _msg;

	free(msg->payload);
	msg->payload = NULL;
	msg->len = 0;
}


void write_can(int lenght, char data[8], int id){

    struct can_frame frame;
    int nbytes,i;

    for(i=0;i<lenght;i++){
    	frame.data[i] = data[i];
    }
    frame.can_dlc = lenght;
    frame.can_id = id;

    nbytes = write(canFD, &frame, sizeof(struct can_frame));

}

void processStringToCan(char *in, int len) {

	int id;
	int dlc;
	char data[20];
	unsigned int test = 0;
	unsigned char canData[8];

	int errorCode = sscanf(in, "%d#%d#%s",&id, &dlc, data);
	if(errorCode <= 0) {
		return;
	}
	printf("id: %d\n", id);
	printf("dlc: %d\n", dlc);
	printf("data: %s\n", data);

	int dataLen = strlen(data);
	printf("len: %d\n", dataLen);


	for(int i = 0; i< dlc; i++) {
		if(i < dlc-1) {
			sscanf(data, "%02X%s", &test, data);
		} else {
			sscanf(data, "%02X", &test);
		}
		canData[i] = test;
		printf("data%d : %d (%02X)\n", i, canData[i], canData[i]);
	}
	write_can(dlc, canData, id);

}



void read_can(int s,struct can_frame frame, struct per_vhost_data__minimal *vhd){
	
	int nbytes,i,n, len = 128, index = 1;
	struct msg amsg;
	char buffer[50]; 
	char* bufferPos = buffer;
    
	
    nbytes = read(s, &frame, sizeof(struct can_frame));

    if (nbytes < 0) {
	    perror("can raw socket read");
	    return;
    }
	
	printf("id: %d\n", frame.can_id);

	bufferPos += sprintf(bufferPos, "%02X#", frame.can_id);
	bufferPos += sprintf(bufferPos, "%d#", frame.can_dlc);

	for (i=0;i<frame.can_dlc;i++){
		printf("%d ",frame.data[i]);
		bufferPos += sprintf(bufferPos, "%02X ",frame.data[i]);
	}
	printf("\n");


	 

	if (vhd->pss_list) {

		pthread_mutex_lock(&vhd->lock_ring); 

		printf("test\n");


		n = (int)lws_ring_get_count_free_elements(vhd->ring);
		if (!n) {
			printf("dropping 1!\n");
		} else {
			len = strlen(buffer);
			amsg.payload = malloc(LWS_PRE + len);
			if (!amsg.payload) {
				printf("OOM: dropping\n");
			} else {
				n = lws_snprintf((char *)amsg.payload + LWS_PRE, len+30, "%s", buffer);
				amsg.len = n;
				n = lws_ring_insert(vhd->ring, &amsg, 1);
				if (n != 1) {
					__minimal_destroy_message(&amsg);
					printf("dropping 2!\n");
				} else {
					lws_cancel_service(vhd->context);
				}
			}
		}
		pthread_mutex_unlock(&vhd->lock_ring);
	}

}



// Encapsulation dans un thread

void* read_can_thread(void* arg) {
	
	// Initialisation du CAN

	struct can_frame frame;
    struct sockaddr_can addr;
    struct ifreq ifr;
	int nbytes,i;
	int loopback = 0; /* 0 = disabled, 1 = enabled (default) */

	struct per_vhost_data__minimal *vhd = (struct per_vhost_data__minimal *)arg;

	canFD = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	
	strcpy(ifr.ifr_name, "slcan0" );
	ioctl(canFD, SIOCGIFINDEX, &ifr);

	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	bind(canFD, (struct sockaddr *)&addr, sizeof(addr));

	setsockopt(canFD, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &loopback, sizeof(loopback));

	while (1) {
		read_can(canFD,frame, vhd);
		usleep(100000);
	}
}




static int
callback_minimal(struct lws *wsi, enum lws_callback_reasons reason,
			void *user, void *in, size_t len)
{
	struct per_session_data__minimal *pss = (struct per_session_data__minimal *)user;
	struct per_vhost_data__minimal *vhd = (struct per_vhost_data__minimal *) lws_protocol_vh_priv_get(lws_get_vhost(wsi), lws_get_protocol(wsi));
	int m;
	const struct msg *pmsg;
	const struct lws_protocol_vhost_options *pvo;

	switch (reason) {
	case LWS_CALLBACK_PROTOCOL_INIT:
		printf("LWS_CALLBACK_PROTOCOL_INIT\n");
		vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
				lws_get_protocol(wsi),
				sizeof(struct per_vhost_data__minimal));
		if (!vhd)
			return 1;

		pthread_mutex_init(&vhd->lock_ring, NULL);

		/* recover the pointer to the globals struct */
		pvo = lws_pvo_search(
			(const struct lws_protocol_vhost_options *)in,
			"config");
		if (!pvo || !pvo->value) {
			lwsl_err("%s: Can't find \"config\" pvo\n", __func__);
			return 1;
		}
		vhd->config = pvo->value;

		vhd->context = lws_get_context(wsi);
		vhd->protocol = lws_get_protocol(wsi);
		vhd->vhost = lws_get_vhost(wsi);

		vhd->ring = lws_ring_create(sizeof(struct msg), 8,
					    __minimal_destroy_message);
		if (!vhd->ring) {
			lwsl_err("%s: failed to create ring\n", __func__);
			return 1;
		}

		pthread_t readThread;


		if(pthread_create(&readThread, NULL,read_can_thread, vhd) == -1) {
			printf("Impossible de creer le thread-lecture");
			return -1;
		} else {
			printf("CAN thread started\n");
		}
		
		break;

	case LWS_CALLBACK_ESTABLISHED:
		printf("LWS_CALLBACK_ESTABLISHED\n");
		/* add ourselves to the list of live pss held in the vhd */
		lws_ll_fwd_insert(pss, pss_list, vhd->pss_list);
		printf("LWS_CALLBACK_ESTABLISHED 2\n");
		pss->tail = lws_ring_get_oldest_tail(vhd->ring);
		printf("LWS_CALLBACK_ESTABLISHED 3\n");
		pss->wsi = wsi;
		
		break;

	case LWS_CALLBACK_CLOSED:
		/* remove our closing pss from the list of live pss */
		lws_ll_fwd_remove(struct per_session_data__minimal, pss_list,
				  pss, vhd->pss_list);
		printf("LWS_CALLBACK_CLOSED\n");
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:
		printf("LWS_CALLBACK_SERVER_WRITEABLE\n");
		pthread_mutex_lock(&vhd->lock_ring); /* --------- ring lock { */

		pmsg = lws_ring_get_element(vhd->ring, &pss->tail);
		if (!pmsg) {
			pthread_mutex_unlock(&vhd->lock_ring); /* } ring lock ------- */
			break;
		}

		/* notice we allowed for LWS_PRE in the payload already */
		m = lws_write(wsi, ((unsigned char *)pmsg->payload) + LWS_PRE,
			      pmsg->len, LWS_WRITE_TEXT);
		if (m < (int)pmsg->len) {
			pthread_mutex_unlock(&vhd->lock_ring); /* } ring lock ------- */
			lwsl_err("ERROR %d writing to ws socket\n", m);
			return -1;
		}

		lws_ring_consume_and_update_oldest_tail(
			vhd->ring,	/* lws_ring object */
			struct per_session_data__minimal, /* type of objects with tails */
			&pss->tail,	/* tail of guy doing the consuming */
			1,		/* number of payload objects being consumed */
			vhd->pss_list,	/* head of list of objects with tails */
			tail,		/* member name of tail in objects with tails */
			pss_list	/* member name of next object in objects with tails */
		);

		/* more to do? */
		if (lws_ring_get_element(vhd->ring, &pss->tail))
			/* come back as soon as we can write more */
			lws_callback_on_writable(pss->wsi);

		pthread_mutex_unlock(&vhd->lock_ring); /* } ring lock ------- */
		
		break;

	case LWS_CALLBACK_RECEIVE:

		printf("%.*s\n", (int)len, (char *)in);
		processStringToCan((char *)in, (int)len);

		printf("LWS_CALLBACK_RECEIVE\n");
		break;

	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		if (!vhd)
			break;
		/*
		 * When the "spam" threads add a message to the ringbuffer,
		 * they create this event in the lws service thread context
		 * using lws_cancel_service().
		 *
		 * We respond by scheduling a writable callback for all
		 * connected clients.
		 */
		lws_start_foreach_llp(struct per_session_data__minimal **,
				      ppss, vhd->pss_list) {
			lws_callback_on_writable((*ppss)->wsi);
		} lws_end_foreach_llp(ppss, pss_list);
		break;

	default:
		break;
	}

	return 0;
}

#define LWS_PLUGIN_PROTOCOL_MINIMAL \
	{ \
		"lws-minimal", \
		callback_minimal, \
		sizeof(struct per_session_data__minimal), \
		128, \
		0, NULL, 0 \
	}

#if !defined (LWS_PLUGIN_STATIC)

/* boilerplate needed if we are built as a dynamic plugin */

static const struct lws_protocols protocols[] = {
	LWS_PLUGIN_PROTOCOL_MINIMAL
};

LWS_EXTERN LWS_VISIBLE int
init_protocol_minimal(struct lws_context *context,
		      struct lws_plugin_capability *c)
{
	if (c->api_magic != LWS_PLUGIN_API_MAGIC) {
		lwsl_err("Plugin API %d, library API %d", LWS_PLUGIN_API_MAGIC,
			 c->api_magic);
		return 1;
	}

	c->protocols = protocols;
	c->count_protocols = LWS_ARRAY_SIZE(protocols);
	c->extensions = NULL;
	c->count_extensions = 0;

	return 0;
}

LWS_EXTERN LWS_VISIBLE int
destroy_protocol_minimal(struct lws_context *context)
{
	return 0;
}
#endif
