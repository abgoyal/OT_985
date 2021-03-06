

#ifndef __BFA_CALLBACK_PRIV_H__
#define __BFA_CALLBACK_PRIV_H__

#include <cs/bfa_q.h>

typedef void    (*bfa_cb_cbfn_t) (void *cbarg, bfa_boolean_t complete);

struct bfa_cb_qe_s {
	struct list_head         qe;
	bfa_cb_cbfn_t  cbfn;
	bfa_boolean_t   once;
	u32		rsvd;
	void           *cbarg;
};

#define bfa_cb_queue(__bfa, __hcb_qe, __cbfn, __cbarg) do {		\
	(__hcb_qe)->cbfn  = (__cbfn);      \
	(__hcb_qe)->cbarg = (__cbarg);      \
	list_add_tail(&(__hcb_qe)->qe, &(__bfa)->comp_q);      \
} while (0)

#define bfa_cb_dequeue(__hcb_qe)	list_del(&(__hcb_qe)->qe)

#define bfa_cb_queue_once(__bfa, __hcb_qe, __cbfn, __cbarg) do {	\
	(__hcb_qe)->cbfn  = (__cbfn);      \
	(__hcb_qe)->cbarg = (__cbarg);      \
	if (!(__hcb_qe)->once) {      \
		list_add_tail((__hcb_qe), &(__bfa)->comp_q);      \
		(__hcb_qe)->once = BFA_TRUE;				\
	}								\
} while (0)

#define bfa_cb_queue_done(__hcb_qe) do {				\
	(__hcb_qe)->once = BFA_FALSE;					\
} while (0)

#endif /* __BFA_CALLBACK_PRIV_H__ */
