/*
 * Copyright (c) 2008 Zmanda, Inc.  All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 as
 * published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA.
 *
 * Contact information: Zmanda Inc., 505 N Mathlida Ave, Suite 120
 * Sunnyvale, CA 94085, USA, or: http://www.zmanda.com
 */

#include "amxfer.h"
#include "element-glue.h"
#include "amanda.h"

/*
 * "Class" declaration
 */

struct Xfer {
    gint refcount;

    /* All transfer elements for this transfer, in order from
     * source to destination.  This is initialized when the Xfer is
     * created. */
    GPtrArray *elements;

    /* The current status of this transfer */
    xfer_status status;

    /* temporary string for a representation of this transfer */
    char *repr;

    /* GSource and queue for incoming messages */
    struct XMsgSource *msg_source;
    GAsyncQueue *queue;

    /* Number of active elements remaining (a.k.a. the number of
     * XMSG_DONE messages to expect) */
    gint num_active_elements;
};

/* XMsgSource objects are GSource "subclasses" which manage
 * a queue of messages, delivering those messages via callback
 * in the mainloop.  Messages can be *sent* from any thread without
 * any concern for locking, but must only be received in the main
 * thread, in the default GMainContext.
 *
 * An XMsgSource pointer can be cast to a GSource pointer as
 * necessary.
 */
typedef struct XMsgSource {
    GSource source; /* must be the first element of the struct */
    Xfer *xfer;
} XMsgSource;

/* forward prototypes */
static XMsgSource *xmsgsource_new(Xfer *xfer);
static void link_elements(Xfer *xfer);

Xfer *
xfer_new(
    XferElement **elements,
    unsigned int nelements)
{
    Xfer *xfer = g_new0(Xfer, 1);
    unsigned int i;

    g_assert(elements);
    g_assert(nelements >= 2);

    xfer->refcount = 1;
    xfer->status = XFER_INIT;
    xfer->repr = NULL;

    /* Create our message source and corresponding queue */
    xfer->msg_source = xmsgsource_new(xfer);
    g_source_ref((GSource *)xfer->msg_source);
    xfer->queue = g_async_queue_new();

    /* copy the elements in, verifying that they're all XferElement objects */
    xfer->elements = g_ptr_array_sized_new(nelements);
    for (i = 0; i < nelements; i++) {
	g_assert(elements[i] != NULL);
	g_assert(IS_XFER_ELEMENT(elements[i]));
	g_assert(elements[i]->xfer == NULL);

	g_ptr_array_add(xfer->elements, (gpointer)elements[i]);

	g_object_ref(elements[i]);
	elements[i]->xfer = xfer;
    }

    return xfer;
}

void
xfer_ref(
    Xfer *xfer)
{
    ++xfer->refcount;
}

void
xfer_unref(
    Xfer *xfer)
{
    unsigned int i;
    XMsg *msg;

    if (!xfer) return; /* be friendly to NULLs */

    if (--xfer->refcount > 0) return;

    g_assert(xfer != NULL);
    g_assert(xfer->status == XFER_INIT || xfer->status == XFER_DONE);

    /* Divorce ourselves from the message source */
    xfer->msg_source->xfer = NULL;
    g_source_unref((GSource *)xfer->msg_source);
    xfer->msg_source = NULL;

    /* Try to empty the message queue */
    while ((msg = (XMsg *)g_async_queue_try_pop(xfer->queue))) {
	g_warning("Dropping XMsg from %s because the XMsgSource is being destroyed", 
	    xfer_element_repr(msg->elt));
	xmsg_free(msg);
    }
    g_async_queue_unref(xfer->queue);

    /* Free our references to the elements, and also set the 'xfer'
     * attribute of each to NULL, making them "unattached" (although 
     * subsequent reuse of elements is untested). */
    for (i = 0; i < xfer->elements->len; i++) {
	XferElement *elt = (XferElement *)g_ptr_array_index(xfer->elements, i);

	elt->xfer = NULL;
	g_object_unref(elt);
    }

    g_free(xfer);
}

GSource *
xfer_get_source(
    Xfer *xfer)
{
    return (GSource *)xfer->msg_source;
}

void
xfer_queue_message(
    Xfer *xfer,
    XMsg *msg)
{
    g_assert(xfer != NULL);
    g_assert(msg != NULL);

    g_async_queue_push(xfer->queue, (gpointer)msg);

    /* TODO: don't do this if we're in the main thread */
    g_main_context_wakeup(NULL);
}

xfer_status
xfer_get_status(Xfer *xfer)
{
    return xfer->status;
}

char *
xfer_repr(
    Xfer *xfer)
{
    unsigned int i;

    if (!xfer->repr) {
	xfer->repr = newvstrallocf(xfer->repr, "<Xfer@%p (", xfer);
	for (i = 0; i < xfer->elements->len; i++) {
	    XferElement *elt = (XferElement *)g_ptr_array_index(xfer->elements, i);
	    xfer->repr = newvstralloc(xfer->repr,
		xfer->repr, (i==0)?"":" -> ", xfer_element_repr(elt), NULL);
	}
	xfer->repr = newvstralloc(xfer->repr, xfer->repr, ")>", NULL);
    }

    return xfer->repr;
}

void
xfer_start(
    Xfer *xfer)
{
    unsigned int len;
    unsigned int i;
    XferElement *xe;

    g_assert(xfer != NULL);
    g_assert(xfer->status == XFER_INIT);
    g_assert(xfer->elements->len >= 2);

    g_debug("Starting %s", xfer_repr(xfer));
    /* set the status to XFER_START and add a reference to our count, so that
     * we are not freed while still in operation.  We'll drop this reference
     * when the status becomes XFER_DONE. */
    xfer_ref(xfer);
    xfer->num_active_elements = 0;
    xfer->status = XFER_START;

    /* check that the first element is an XferSource and the last is an XferDest.
     * A source is identified by having no input mechanisms. */
    xe = (XferElement *)g_ptr_array_index(xfer->elements, 0);
    if (XFER_ELEMENT_GET_CLASS(xe)->mech_pairs[0].input_mech != XFER_MECH_NONE)
	error("Transfer element 0 is not a transfer source");

    /* Similarly, a destination has no output mechanisms. */
    xe = (XferElement *)g_ptr_array_index(xfer->elements, xfer->elements->len-1);
    if (XFER_ELEMENT_GET_CLASS(xe)->mech_pairs[0].output_mech != XFER_MECH_NONE)
	error("Last transfer element is not a transfer destination");

    /* Link the elements.  This calls error() on failure, and rewrites
     * xfer->elements */
    link_elements(xfer);

    /* Tell all elements to set up.  This is done before upstream and downstream
     * are set so that elements cannot interfere with one another before setup()
     * is completed. */
    for (i = 0; i < xfer->elements->len; i++) {
	XferElement *xe = (XferElement *)g_ptr_array_index(xfer->elements, i);
	xfer_element_setup(xe);
    }

    /* Set the upstream and downstream links between elements */
    len = xfer->elements->len;
    for (i = 0; i < len; i++) {
	XferElement *elt = g_ptr_array_index(xfer->elements, i);

	if (i > 0)
	    elt->upstream = g_ptr_array_index(xfer->elements, i-1);
	if (i < len-1)
	    elt->downstream = g_ptr_array_index(xfer->elements, i+1);
    }

    /* now tell them all to start, in order from destination to source */
    for (i = xfer->elements->len; i >= 1; i--) {
	XferElement *xe = (XferElement *)g_ptr_array_index(xfer->elements, i-1);
	if (xfer_element_start(xe))
	    xfer->num_active_elements++;
    }

    xfer->status = XFER_RUNNING;

    /* If this transfer involves no active processing, then we consider it to
     * be done already.  We send a "fake" XMSG_DONE from the destination element,
     * so that all of the usual processing will take place. */
    if (xfer->num_active_elements == 0) {
	g_debug("%s has no active elements; generating fake XMSG_DONE", xfer_repr(xfer));
	xfer->num_active_elements++;
	xfer_queue_message(xfer,
	    xmsg_new((XferElement *)g_ptr_array_index(xfer->elements, xfer->elements->len-1),
		     XMSG_DONE, 0));
    }
}

/*
 * Element linking
 */

/* How is ELT linked? link_recurse uses an array of these to track its progress
 * and find the optimal overall linkage. */
typedef struct linkage {
    XferElement *elt;
    int elt_idx; /* index into elt's mech_pairs */
    int glue_idx; /* index into glue pairs for elt's output; -1 = no glue */
} linkage;

/* Overall state of the recursive linking process */
typedef struct linking_state {
    int nlinks; /* number of linkage objects in each array */
    linkage *cur; /* "current" linkage */

    linkage *best; /* best linkage so far */
    gint32 best_cost; /* cost for best */
} linking_state;

/* used for debugging messages */
static char *
xfer_mech_name(
    xfer_mech mech)
{
    switch (mech) {
	case XFER_MECH_NONE: return "NONE";
	case XFER_MECH_READFD: return "READFD";
	case XFER_MECH_WRITEFD: return "WRITEFD";
	case XFER_MECH_PULL_BUFFER: return "PULL_BUFFER";
	case XFER_MECH_PUSH_BUFFER: return "PUSH_BUFFER";
	default: return "UNKNOWN";
    }
}

/* calculate an integer representing the cost of a mech pair as a
 * single integer.  OPS_PER_BYTE is the most important metric,
 * followed by NTHREADS.
 *
 * PAIR will be evaluated multiple times.
 */
#define PAIR_COST(pair) (((pair).ops_per_byte << 8) + (pair).nthreads)

/* maximum cost */
#define MAX_COST 0xffffff

/* Generate all possible linkages of elements [idx:nlinks], where
 * elements [0:idx-1] have cost 'cost' and end with mechanism
 * 'input_mech'. */
static void
link_recurse(
    linking_state *st,
    int idx,
    xfer_mech input_mech,
    gint32 cost)
{
    xfer_element_mech_pair_t *elt_pairs, *glue_pairs;
    linkage *my;

    /* if we've overrun the previous best cost already, then bail out */
    if (cost >= st->best_cost)
	return;

    /* have we linked everything? */
    if (idx == st->nlinks) {
	/* if we ended on other than XFER_MECH_NONE, then this is not a
	 * valid transfer */
	if (input_mech != XFER_MECH_NONE) return;

	/* we already know this has lower cost than the previous best */
	memcpy(st->best, st->cur, st->nlinks * sizeof(linkage));
	st->best_cost = cost;

	return;
    }

    /* recurse for each linkage we can make that starts with input_mech */
    my = &st->cur[idx];
    elt_pairs = XFER_ELEMENT_GET_CLASS(my->elt)->mech_pairs;
    glue_pairs = xfer_element_glue_mech_pairs;

    for (my->elt_idx = 0;
	 elt_pairs[my->elt_idx].input_mech != XFER_MECH_NONE
	 || elt_pairs[my->elt_idx].output_mech != XFER_MECH_NONE;
	 my->elt_idx++) {
	 /* reject this pair if the input mech does not match */
	 if (elt_pairs[my->elt_idx].input_mech != input_mech)
	    continue;

	 /* recurse with no glue */
	 my->glue_idx = -1;
	 link_recurse(st, idx+1,
		      elt_pairs[my->elt_idx].output_mech,
		      cost + PAIR_COST(elt_pairs[my->elt_idx]));

	/* and recurse with glue */
	for (my->glue_idx = 0;
	     glue_pairs[my->glue_idx].input_mech != XFER_MECH_NONE
	     || glue_pairs[my->glue_idx].output_mech != XFER_MECH_NONE;
	     my->glue_idx++) {
	    /* reject this glue pair if it doesn't match with the element output */
	    if (glue_pairs[my->glue_idx].input_mech != elt_pairs[my->elt_idx].output_mech)
		continue;

	     /* and recurse with the glue */
	     link_recurse(st, idx+1,
			  glue_pairs[my->glue_idx].output_mech,
			  cost + PAIR_COST(elt_pairs[my->elt_idx])
			       + PAIR_COST(glue_pairs[my->glue_idx]));
	}
    }
}

static void
link_elements(
    Xfer *xfer)
{
    GPtrArray *new_elements;
    XferElement *elt;
    XferElementClass *eltc;
    char *linkage_str;
    linking_state st;
    gint i, len;

    /* Note that this algorithm's running time is polynomial in the length of
     * the transfer, with a fairly high order.  If Amanda is regularly assembling
     * transfers with more than, say, 6 elements, then the algorithm should be
     * redesigned. */

    /* set up the state for recursion */
    st.nlinks = xfer->elements->len;
    st.cur = g_new0(linkage, st.nlinks);
    st.best = g_new0(linkage, st.nlinks);
    st.best_cost = MAX_COST;
    for (i = 0; i < st.nlinks; i++) {
	st.cur[i].elt = (XferElement *)g_ptr_array_index(xfer->elements, i);
    }

    /* start recursing with the first element, asserting that its input mech is NONE */
    link_recurse(&st, 0, XFER_MECH_NONE, 0);

    /* check that we got *some* solution */
    if (st.best_cost == MAX_COST) {
	error(_("Xfer %s cannot be linked."), xfer_repr(xfer));
    }

    /* Now create a new list of elements, containing any glue elements
     * that we need to add, and set their input_mech and output_mech fields */
    new_elements = g_ptr_array_sized_new(xfer->elements->len);
    for (i = 0; i < st.nlinks; i++) {
	elt = st.best[i].elt;
	eltc = XFER_ELEMENT_GET_CLASS(elt);
	elt->input_mech = eltc->mech_pairs[st.best[i].elt_idx].input_mech;
	elt->output_mech = eltc->mech_pairs[st.best[i].elt_idx].output_mech;
	g_ptr_array_add(new_elements, elt);

	if (st.best[i].glue_idx != -1) {
	    elt = xfer_element_glue();
	    eltc = XFER_ELEMENT_GET_CLASS(elt);
	    elt->xfer = xfer;
	    elt->input_mech = eltc->mech_pairs[st.best[i].glue_idx].input_mech;
	    elt->output_mech = eltc->mech_pairs[st.best[i].glue_idx].output_mech;
	    g_ptr_array_add(new_elements, elt);
	}
    }

    /* install the new list of elements */
    g_ptr_array_free(xfer->elements, FALSE);
    xfer->elements = new_elements;
    new_elements = NULL;

    /* debug-log the xfer's linkage */
    len = xfer->elements->len;
    linkage_str = stralloc("Final linkage: ");
    for (i = 0; i < len; i++) {
	XferElement *elt = g_ptr_array_index(xfer->elements, i);

	if (i == 0)
	    linkage_str = newvstralloc(linkage_str, linkage_str, xfer_element_repr(elt), NULL);
	else
	    linkage_str = newvstrallocf(linkage_str, "%s -(%s)-> %s",
		linkage_str, xfer_mech_name(elt->input_mech), xfer_element_repr(elt));
    }
    g_debug("%s", linkage_str);
    amfree(linkage_str);

    amfree(st.cur);
    amfree(st.best);
}

/*
 * XMsgSource
 */

static gboolean
xmsgsource_prepare(
    GSource *source,
    gint *timeout_)
{
    XMsgSource *xms = (XMsgSource *)source;

    *timeout_ = -1;
    return xms->xfer && g_async_queue_length(xms->xfer->queue) > 0;
}

static gboolean
xmsgsource_check(
    GSource *source)
{
    XMsgSource *xms = (XMsgSource *)source;

    return xms->xfer && g_async_queue_length(xms->xfer->queue) > 0;
}

static gboolean
xmsgsource_dispatch(
    GSource *source G_GNUC_UNUSED,
    GSourceFunc callback,
    gpointer user_data)
{
    XMsgSource *xms = (XMsgSource *)source;
    XMsgCallback my_cb = (XMsgCallback)callback;
    XMsg *msg;
    gboolean deliver_to_caller;

    /* we're calling perl within this loop, so we have to check that everything is
     * ok on each iteration of the loop. */
    while (xms->xfer
        && xms->xfer->status == XFER_RUNNING
        && (msg = (XMsg *)g_async_queue_try_pop(xms->xfer->queue))) {
	/* We get first crack at interpreting messages, before calling the
	 * designated callback. */

	deliver_to_caller = TRUE;
	switch (msg->type) {
	    /* Intercept and count DONE messages so that we can determine when
	     * the entire transfer is finished. */
	    case XMSG_DONE:
		if (--xms->xfer->num_active_elements <= 0) {
		    /* mark the transfer as done, and decrement the refcount
		     * by one to balance the increment in xfer_start */
		    xms->xfer->status = XFER_DONE;
		    xfer_unref(xms->xfer);
		} else {
		    /* eat this XMSG_DONE, since we expect more */
		    deliver_to_caller = FALSE;
		}
		break;

	    default:
		break;  /* nothing interesting to do */
	}

	if (deliver_to_caller) {
	    if (my_cb) {
		my_cb(user_data, msg, xms->xfer);
	    } else {
		g_warning("Dropping %s because no callback is set", xmsg_repr(msg));
	    }
	}

	xmsg_free(msg);
    }

    /* Never automatically un-queue the event source */
    return TRUE;
}

XMsgSource *
xmsgsource_new(
    Xfer *xfer)
{
    static GSourceFuncs *xmsgsource_funcs = NULL;
    GSource *src;
    XMsgSource *xms;

    /* initialize these here to avoid a compiler warning */
    if (!xmsgsource_funcs) {
	xmsgsource_funcs = g_new0(GSourceFuncs, 1);
	xmsgsource_funcs->prepare = xmsgsource_prepare;
	xmsgsource_funcs->check = xmsgsource_check;
	xmsgsource_funcs->dispatch = xmsgsource_dispatch;
    }

    src = g_source_new(xmsgsource_funcs, sizeof(XMsgSource));
    xms = (XMsgSource *)src;
    xms->xfer = xfer;

    return xms;
}