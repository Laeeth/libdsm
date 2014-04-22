//---------------------------------------------------------------------------
//  __________________    _________  _____            _____  .__         ._.
//  \______   \______ \  /   _____/ /     \          /  _  \ |__| ____   | |
//   |    |  _/|    |  \ \_____  \ /  \ /  \        /  /_\  \|  _/ __ \  | |
//   |    |   \|    `   \/        /    Y    \      /    |    |  \  ___/   \|
//   |______  /_______  /_______  \____|__  / /\   \____|__  |__|\___ |   __
//          \/        \/        \/        \/  )/           \/        \/   \/
//
// This file is part of libdsm. Copyright © 2014 VideoLabs SAS
//
// Author: Julien 'Lta' BALLET <contact@lta.io>
//
// This program is free software. It comes without any warranty, to the extent
// permitted by applicable law. You can redistribute it and/or modify it under
// the terms of the Do What The Fuck You Want To Public License, Version 2, as
// published by Sam Hocevar. See the COPYING file for more details.
//----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "bdsm/debug.h"
#include "bdsm/smb_session.h"
#include "bdsm/smb_ntlm.h"

smb_session_t   *smb_session_new()
{
  smb_session_t *s;

  s = malloc(sizeof(smb_session_t));
  assert(s != NULL);
  memset((void *)s, 0, sizeof(smb_session_t));

  return (s);
}

void            smb_session_destroy(smb_session_t *s)
{
  if (s != NULL)
  {
    // FIXME Free smb_share and smb_file
    if (s->nb_session != NULL)
      netbios_session_destroy(s->nb_session);
    free(s);
  }
}

int             smb_session_state(smb_session_t *s)
{
  if (s != NULL)
    return (s->state);
  else
    return (SMB_STATE_ERROR);
}

int             smb_session_connect(smb_session_t *s, const char *name,
                                    uint32_t ip)
{
  assert(s != NULL && name != NULL);

  if ((s->nb_session = netbios_session_new(ip)) == NULL)
    goto error;
  if (!netbios_session_connect(s->nb_session, name))
    goto error;

  memcpy(s->srv.name, name, strlen(name) + 1);
  s->state = SMB_STATE_NETBIOS_OK;

  if (!smb_negotiate(s))
    return (0);

  return(1);

  error:
    s->state = SMB_STATE_ERROR;
    return (0);
}

int             smb_session_send_msg(smb_session_t *s, smb_message_t *msg)
{
  size_t        packet_size;

  assert(s != NULL && s->state >= SMB_STATE_NETBIOS_OK && s->nb_session != NULL);
  assert(msg != NULL && msg->packet != NULL);

  msg->packet->header.uid = s->uid;

  netbios_session_packet_init(s->nb_session, NETBIOS_OP_SESSION_MSG);

  packet_size   = sizeof(smb_packet_t) + msg->cursor;
  if (!netbios_session_packet_append(s->nb_session, (char *)msg->packet, packet_size))
    return (0);
  if (!netbios_session_packet_send(s->nb_session))
    return (0);

  return (1);
}

size_t          smb_session_recv_msg(smb_session_t *s, smb_message_t *msg)
{
  netbios_session_packet_t  *nb_packet;
  ssize_t                   recv_size;
  size_t                    payload_size;

  assert(s != NULL && s->nb_session != NULL);

  recv_size = netbios_session_packet_recv(s->nb_session);
  if(recv_size <= 0)
    return (0);

  nb_packet   = (netbios_session_packet_t *)s->nb_session->recv_buffer;
  if (msg != NULL)
    msg->packet = (smb_packet_t *)nb_packet->payload;

  payload_size  = ntohs(nb_packet->length);
  payload_size |= (nb_packet->flags & 0x01) << 16; // XXX If this is the case we overran our recv_buffer
  if (payload_size > recv_size - sizeof(netbios_session_packet_t))
  {
    BDSM_dbg("smb_session_recv_msg: Packet size mismatch\n");
    return(0);
  }

  if (msg != NULL)
  {
    msg->payload_size = payload_size - sizeof(smb_header_t);
    msg->cursor       = 0;
  }

  return (payload_size - sizeof(smb_header_t));
}


void        smb_session_share_add(smb_session_t *s, smb_share_t *share)
{
  smb_share_t *iter;

  assert(s != NULL && share != NULL);

  if (s->shares == NULL)
  {
    s->shares = share;
    return;
  }

  iter = s->shares;
  while(iter->next != NULL)
    iter = iter->next;
  iter->next = share;
}

smb_share_t *smb_session_share_get(smb_session_t *s, smb_tid tid)
{
  smb_share_t *iter;

  assert(s != NULL && tid);

  iter = s->shares;
  while(iter != NULL && iter->tid != tid)
    iter = iter->next;

  return (iter);
}

smb_share_t *smb_session_share_remove(smb_session_t *s, smb_tid tid)
{
  smb_share_t *iter, *keep;

  assert(s != NULL && tid);
  iter = s->shares;

  if (iter == NULL)
    return (NULL);
  if (iter->tid == tid)
  {
    s->shares = s->shares->next;
    return (iter);
  }

  while(iter->next != NULL && iter->next->tid != tid)
    iter = iter->next;

  if (iter->next != NULL) // We found it
  {
    keep = iter->next;
    iter->next = iter->next->next;
    return (keep);
  }
  return (NULL);
}

int         smb_session_file_add(smb_session_t *s, smb_tid tid, smb_file_t *f)
{
  smb_share_t *share;
  smb_file_t  *iter;

  assert(s != NULL && tid && f != NULL);

  if ((share = smb_session_share_get(s, tid)) == NULL)
    return (0);

  if (share->files == NULL)
    share->files = f;
  else
  {
    iter = share->files;
    while (iter->next != NULL)
      iter = iter->next;
    iter->next = f;
  }

  return (1);
}

smb_file_t  *smb_session_file_get(smb_session_t *s, smb_fd fd)
{
  smb_share_t *share;
  smb_file_t  *iter;

  assert(s != NULL && fd);

  if ((share = smb_session_share_get(s, SMB_FD_TID(fd))) == NULL)
    return (NULL);

  iter = share->files;
  while(iter != NULL && iter->fid != SMB_FD_FID(fd))
    iter = iter->next;

  return (iter);
}

smb_file_t  *smb_session_file_remove(smb_session_t *s, smb_fd fd)
{
  smb_share_t *share;
  smb_file_t  *iter, *keep;

  assert(s != NULL && fd);

  if ((share = smb_session_share_get(s, SMB_FD_TID(fd))) == NULL)
    return (NULL);

  iter = share->files;

  if (iter == NULL)
    return (NULL);
  if (iter->fid == SMB_FD_FID(fd))
  {
    share->files = iter->next;
    return (iter);
  }

  while(iter->next != NULL && iter->next->fid != SMB_FD_TID(fd))
    iter = iter->next;
  if (iter->next != NULL)
  {
    keep = iter->next;
    iter->next = iter->next->next;
    return (keep);
  }
  else
    return (NULL);
}



int             smb_negotiate(smb_session_t *s)
{
  const char            *dialects[] = SMB_DIALECTS;
  smb_message_t         *msg = NULL;
  smb_message_t         answer;
  smb_negotiate_resp_t  *nego;


  msg = smb_message_new(SMB_CMD_NEGOTIATE, 64);
  smb_message_set_default_flags(msg);

  smb_message_put8(msg, 0);   // wct
  smb_message_put16(msg, 0);  // bct, will be updated later

  for(unsigned i = 0; dialects[i] != NULL; i++)
    smb_message_append(msg, dialects[i], strlen(dialects[i]) + 1);
  *((uint16_t *)(msg->packet->payload + 1)) = msg->cursor - 3;

  if (!smb_session_send_msg(s, msg))
  {
    smb_message_destroy(msg);
    goto error;
  }
  smb_message_destroy(msg);

  if (!smb_session_recv_msg(s, &answer))
    goto error;

  nego = (smb_negotiate_resp_t *)answer.packet->payload;
  if (answer.packet->header.status != NT_STATUS_SUCCESS
      && nego->wct != 0x11 && nego->security_mode & 0x03)
    goto error;

  s->srv.dialect        = nego->dialect_index;
  s->srv.security_mode  = nego->security_mode;
  s->srv.caps           = nego->caps;
  s->srv.session_key    = nego->session_key;
  s->srv.challenge      = nego->challenge;
  s->srv.ts             = nego->ts;

  // Yeah !
  s->state              = SMB_STATE_DIALECT_OK;

  return (1);

  error:
    s->state = SMB_STATE_ERROR;
    return (0);
}

int             smb_authenticate(smb_session_t *s, const char *domain,
                                 const char *user, const char *password)
{
  smb_message_t         answer;
  smb_message_t         *msg = NULL;
  smb_session_req_t     *req = NULL;
  uint8_t               *ntlm2 = NULL;
  smb_ntlmh_t           hash_v2;
  uint8_t               *ucs;
  size_t                ucs_len;
  uint64_t              user_challenge;
  uint8_t               blob[128];
  size_t                blob_size;

  msg = smb_message_new(SMB_CMD_SETUP, 512);
  smb_message_set_default_flags(msg);
  smb_message_set_andx_members(msg);
  req = (smb_session_req_t *)msg->packet->payload;

  req->wct              = (sizeof(smb_session_req_t) - 3) / 2;
  req->max_buffer       = NETBIOS_SESSION_PAYLOAD;
  req->max_buffer      -= sizeof(netbios_session_packet_t);
  req->max_buffer      -= sizeof(smb_packet_t);
  req->mpx_count        = 16; // XXX ?
  req->vc_count         = 1;
  req->session_key      = s->srv.session_key;
  req->caps             = s->srv.caps; // XXX caps & our_caps_mask

  smb_message_advance(msg, sizeof(smb_session_req_t));

  user_challenge = smb_ntlm_generate_challenge();

  // LM2 Response
  smb_ntlm2_hash(user, password, domain, &hash_v2);
  ntlm2 = smb_ntlm2_response(&hash_v2, s->srv.challenge,
                             (void *)&user_challenge, 8);
  smb_message_append(msg, ntlm2, 16 + 8);
  free(ntlm2);

  // NTLM2 Response
  blob_size = smb_ntlm_blob((smb_ntlm_blob_t *)blob, s->srv.ts,
                            user_challenge, domain);
  ntlm2 = smb_ntlm2_response(&hash_v2, s->srv.challenge, blob, blob_size);
  //smb_message_append(msg, ntlm2, 16 + blob_size);
  free(ntlm2);

  req->oem_pass_len = 16 + SMB_LM2_BLOB_SIZE;
  req->uni_pass_len = 0; //16 + blob_size; //SMB_NTLM2_BLOB_SIZE;
  if (msg->cursor / 2) // Padding !
    smb_message_put8(msg, 0);

  smb_message_put_utf16(msg, "", user, strlen(user));
  smb_message_put16(msg, 0);
  smb_message_put_utf16(msg, "", domain, strlen(domain));
  smb_message_put16(msg, 0);
  smb_message_put_utf16(msg, "", SMB_OS, strlen(SMB_OS));
  smb_message_put16(msg, 0);
  smb_message_put_utf16(msg, "", SMB_LANMAN, strlen(SMB_OS));
  smb_message_put16(msg, 0);

  req->payload_size = msg->cursor - sizeof(smb_session_req_t);

  if (!smb_session_send_msg(s, msg))
  {
    smb_message_destroy(msg);
    BDSM_dbg("Unable to send Session Setup AndX message\n");
    return (0);
  }
  smb_message_destroy(msg);

  if (smb_session_recv_msg(s, &answer) == 0)
  {
    BDSM_dbg("Unable to get Session Setup AndX reply\n");
    return (0);
  }

  smb_session_resp_t *r = (smb_session_resp_t *)answer.packet->payload;
  if (answer.packet->header.status != NT_STATUS_SUCCESS)
  {
    BDSM_dbg("Session Setup AndX : failure.\n");
    return (0);
  }

  if (r->action & 0x0001)
    s->guest = 1;
  s->state  = SMB_STATE_SESSION_OK;
  s->uid    = answer.packet->header.uid;

  return (1);
}
