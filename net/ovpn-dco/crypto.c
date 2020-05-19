// SPDX-License-Identifier: GPL-2.0-only
/*  OpenVPN data channel accelerator
 *
 *  Copyright (C) 2020 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include "main.h"
#include "aead.h"
#include "crypto.h"

#include <uapi/linux/ovpn_dco.h>

static struct ovpn_crypto_key_slot *
ovpn_ks_new(const struct ovpn_crypto_ops *ops, const struct ovpn_key_config *kc)
{
	return ops->new(kc);
}

static void ovpn_ks_destroy_rcu(struct rcu_head *head)
{
	struct ovpn_crypto_key_slot *ks;

	ks = container_of(head, struct ovpn_crypto_key_slot, rcu);
	ks->ops->destroy(ks);
}

void ovpn_crypto_key_slot_release(struct kref *kref)
{
	struct ovpn_crypto_key_slot *ks;

	ks = container_of(kref, struct ovpn_crypto_key_slot, refcount);
	call_rcu(&ks->rcu, ovpn_ks_destroy_rcu);
}

/* can only be invoked when all peer references have been dropped (i.e. RCU
 * release routine)
 */
void ovpn_crypto_state_release(struct ovpn_crypto_state *cs)
{
	struct ovpn_crypto_key_slot *ks;

	ks = rcu_access_pointer(cs->primary);
	if (ks) {
		RCU_INIT_POINTER(cs->primary, NULL);
		ovpn_crypto_key_slot_put(ks);
	}

	ks = rcu_access_pointer(cs->secondary);
	if (ks) {
		RCU_INIT_POINTER(cs->secondary, NULL);
		ovpn_crypto_key_slot_put(ks);
	}

	mutex_destroy(&cs->mutex);
}

int ovpn_crypto_encap_overhead(const struct ovpn_crypto_state *cs)
{
	const struct ovpn_crypto_key_slot *ks;
	int ret;

	rcu_read_lock();
	ks = rcu_dereference(cs->primary);
	if (!ks) {
		rcu_read_unlock();
		return -ENOENT;
	}
	ret = ks->ops->encap_overhead(ks);
	rcu_read_unlock();

	return ret;
}

/* Reset the ovpn_crypto_state object in a way that is atomic
 * to RCU readers.
 */
int ovpn_crypto_state_reset(struct ovpn_crypto_state *cs,
			    const struct ovpn_peer_key_reset *pkr)
	__must_hold(cs->mutex)
{
	struct ovpn_crypto_key_slot *old = NULL;
	struct ovpn_crypto_key_slot *new;

	lockdep_assert_held(&cs->mutex);

	new = ovpn_ks_new(cs->ops, &pkr->key);
	if (IS_ERR(new))
		return PTR_ERR(new);

	new->remote_peer_id = pkr->remote_peer_id;

	switch (pkr->slot) {
	case OVPN_KEY_SLOT_PRIMARY:
		old = rcu_replace_pointer(cs->primary, new,
					  lockdep_is_held(&cs->mutex));
		break;
	case OVPN_KEY_SLOT_SECONDARY:
		old = rcu_replace_pointer(cs->secondary, new,
					  lockdep_is_held(&cs->mutex));
		break;
	default:
		goto free_key;
	}

	pr_debug("*** NEW KEY INSTALLED id=%u remote_pid=%u\n",
		 new->key_id, new->remote_peer_id);

	if (old)
		ovpn_crypto_key_slot_put(old);

	return 0;
free_key:
	ovpn_crypto_key_slot_put(new);
	return -EINVAL;
}

void ovpn_crypto_key_slot_delete(struct ovpn_crypto_state *cs,
				 enum ovpn_key_slot slot)
{
	struct ovpn_crypto_key_slot *ks = NULL;

	mutex_lock(&cs->mutex);
	switch (slot) {
	case OVPN_KEY_SLOT_PRIMARY:
		ks = rcu_replace_pointer(cs->primary, NULL,
					 lockdep_is_held(&cs->mutex));
		break;
	case OVPN_KEY_SLOT_SECONDARY:
		ks = rcu_replace_pointer(cs->secondary, NULL,
					 lockdep_is_held(&cs->mutex));
		break;
	default:
		pr_warn("Invalid slot to release: %u\n", slot);
		break;
	}
	mutex_unlock(&cs->mutex);

	if (!ks) {
		pr_debug("Key slot already released: %u\n", slot);
		return;
	}

	ovpn_crypto_key_slot_put(ks);
}

static const struct ovpn_crypto_ops *
ovpn_crypto_select_family(const struct ovpn_peer_key_reset *pkr)
{
	switch (pkr->crypto_family) {
	case OVPN_CRYPTO_FAMILY_UNDEF:
		return NULL;
	case OVPN_CRYPTO_FAMILY_AEAD:
		return &ovpn_aead_ops;
//	case OVPN_CRYPTO_FAMILY_CBC_HMAC:
//		return &ovpn_chm_ops;
	default:
		return NULL;
	}
}

int ovpn_crypto_state_select_family(struct ovpn_crypto_state *cs,
				    const struct ovpn_peer_key_reset *pkr)
	__must_hold(cs->mutex)
{
	const struct ovpn_crypto_ops *new_ops;

	lockdep_assert_held(&cs->mutex);

	new_ops = ovpn_crypto_select_family(pkr);
	if (!new_ops)
		return -EOPNOTSUPP;

	if (cs->ops && cs->ops != new_ops) /* family changed? */
		return -EINVAL;

	cs->ops = new_ops;

	return 0;
}

enum ovpn_crypto_families
ovpn_keys_familiy_get(const struct ovpn_key_config *kc)
{
	switch (kc->cipher_alg) {
	case OVPN_CIPHER_ALG_AES_GCM:
		return OVPN_CRYPTO_FAMILY_AEAD;
	case OVPN_CIPHER_ALG_AES_CBC:
		return OVPN_CRYPTO_FAMILY_CBC_HMAC;
	default:
		return OVPN_CRYPTO_FAMILY_UNDEF;
	}
}
