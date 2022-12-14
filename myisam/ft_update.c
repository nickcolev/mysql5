/* Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* Written by Sergei A. Golubchik, who has a shared copyright to this code */

/* functions to work with full-text indices */

#include "ftdefs.h"
#include <math.h>

void _mi_ft_segiterator_init(MI_INFO *info, uint keynr, const byte *record,
			     FT_SEG_ITERATOR *ftsi)
{
  DBUG_ENTER("_mi_ft_segiterator_init");

  ftsi->num=info->s->keyinfo[keynr].keysegs;
  ftsi->seg=info->s->keyinfo[keynr].seg;
  ftsi->rec=record;
  DBUG_VOID_RETURN;
}

void _mi_ft_segiterator_dummy_init(const byte *record, uint len,
				   FT_SEG_ITERATOR *ftsi)
{
  DBUG_ENTER("_mi_ft_segiterator_dummy_init");

  ftsi->num=1;
  ftsi->seg=0;
  ftsi->pos=record;
  ftsi->len=len;
  DBUG_VOID_RETURN;
}

/*
  This function breaks convention "return 0 in success"
  but it's easier to use like this

     while(_mi_ft_segiterator())

  so "1" means "OK", "0" means "EOF"
*/

uint _mi_ft_segiterator(register FT_SEG_ITERATOR *ftsi)
{
  DBUG_ENTER("_mi_ft_segiterator");

  if (!ftsi->num)
    DBUG_RETURN(0);

  ftsi->num--;
  if (!ftsi->seg)
    DBUG_RETURN(1);

  ftsi->seg--;

  if (ftsi->seg->null_bit &&
      (ftsi->rec[ftsi->seg->null_pos] & ftsi->seg->null_bit))
  {
    ftsi->pos=0;
    DBUG_RETURN(1);
  }
  ftsi->pos= ftsi->rec+ftsi->seg->start;
  if (ftsi->seg->flag & HA_VAR_LENGTH_PART)
  {
    uint pack_length= (ftsi->seg->bit_start);
    ftsi->len= (pack_length == 1 ? (uint) *(uchar*) ftsi->pos :
                uint2korr(ftsi->pos)); 
    ftsi->pos+= pack_length;			 /* Skip VARCHAR length */
    DBUG_RETURN(1);
  }
  if (ftsi->seg->flag & HA_BLOB_PART)
  {
    ftsi->len=_mi_calc_blob_length(ftsi->seg->bit_start,ftsi->pos);
    memcpy_fixed((char*) &ftsi->pos, ftsi->pos+ftsi->seg->bit_start,
		 sizeof(char*));
    DBUG_RETURN(1);
  }
  ftsi->len=ftsi->seg->length;
  DBUG_RETURN(1);
}


/* parses a document i.e. calls ft_parse for every keyseg */

uint _mi_ft_parse(TREE *parsed, MI_INFO *info, uint keynr,
                  const byte *record, my_bool with_alloc)
{
  FT_SEG_ITERATOR ftsi;
  DBUG_ENTER("_mi_ft_parse");

  _mi_ft_segiterator_init(info, keynr, record, &ftsi);

  ft_parse_init(parsed, info->s->keyinfo[keynr].seg->charset);
  while (_mi_ft_segiterator(&ftsi))
  {
    if (ftsi.pos)
      if (ft_parse(parsed, (byte *)ftsi.pos, ftsi.len, with_alloc))
        DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}

FT_WORD * _mi_ft_parserecord(MI_INFO *info, uint keynr, const byte *record)
{
  TREE ptree;
  DBUG_ENTER("_mi_ft_parserecord");

  bzero((char*) &ptree, sizeof(ptree));
  if (_mi_ft_parse(&ptree, info, keynr, record,0))
    DBUG_RETURN(NULL);

  DBUG_RETURN(ft_linearize(&ptree));
}

static int _mi_ft_store(MI_INFO *info, uint keynr, byte *keybuf,
			FT_WORD *wlist, my_off_t filepos)
{
  uint key_length;
  DBUG_ENTER("_mi_ft_store");

  for (; wlist->pos; wlist++)
  {
    key_length=_ft_make_key(info,keynr,keybuf,wlist,filepos);
    if (_mi_ck_write(info,keynr,(uchar*) keybuf,key_length))
      DBUG_RETURN(1);
   }
   DBUG_RETURN(0);
}

static int _mi_ft_erase(MI_INFO *info, uint keynr, byte *keybuf,
			FT_WORD *wlist, my_off_t filepos)
{
  uint key_length, err=0;
  DBUG_ENTER("_mi_ft_erase");

  for (; wlist->pos; wlist++)
  {
    key_length=_ft_make_key(info,keynr,keybuf,wlist,filepos);
    if (_mi_ck_delete(info,keynr,(uchar*) keybuf,key_length))
      err=1;
   }
   DBUG_RETURN(err);
}

/*
  Compares an appropriate parts of two WORD_KEY keys directly out of records
  returns 1 if they are different
*/

#define THOSE_TWO_DAMN_KEYS_ARE_REALLY_DIFFERENT 1
#define GEE_THEY_ARE_ABSOLUTELY_IDENTICAL	 0

int _mi_ft_cmp(MI_INFO *info, uint keynr, const byte *rec1, const byte *rec2)
{
  FT_SEG_ITERATOR ftsi1, ftsi2;
  CHARSET_INFO *cs=info->s->keyinfo[keynr].seg->charset;
  DBUG_ENTER("_mi_ft_cmp");
#ifndef MYSQL_HAS_TRUE_CTYPE_IMPLEMENTATION
  if (cs->mbmaxlen > 1)
    DBUG_RETURN(THOSE_TWO_DAMN_KEYS_ARE_REALLY_DIFFERENT);
#endif

  _mi_ft_segiterator_init(info, keynr, rec1, &ftsi1);
  _mi_ft_segiterator_init(info, keynr, rec2, &ftsi2);

  while (_mi_ft_segiterator(&ftsi1) && _mi_ft_segiterator(&ftsi2))
  {
    if ((ftsi1.pos != ftsi2.pos) &&
        (!ftsi1.pos || !ftsi2.pos ||
         mi_compare_text(cs, (uchar*) ftsi1.pos,ftsi1.len,
                         (uchar*) ftsi2.pos,ftsi2.len,0,0)))
      DBUG_RETURN(THOSE_TWO_DAMN_KEYS_ARE_REALLY_DIFFERENT);
  }
  DBUG_RETURN(GEE_THEY_ARE_ABSOLUTELY_IDENTICAL);
}


/* update a document entry */

int _mi_ft_update(MI_INFO *info, uint keynr, byte *keybuf,
                  const byte *oldrec, const byte *newrec, my_off_t pos)
{
  int error= -1;
  FT_WORD *oldlist,*newlist, *old_word, *new_word;
  CHARSET_INFO *cs=info->s->keyinfo[keynr].seg->charset;
  uint key_length;
  int cmp, cmp2;
  DBUG_ENTER("_mi_ft_update");

  if (!(old_word=oldlist=_mi_ft_parserecord(info, keynr, oldrec)))
    goto err0;
  if (!(new_word=newlist=_mi_ft_parserecord(info, keynr, newrec)))
    goto err1;

  error=0;
  while(old_word->pos && new_word->pos)
  {
    cmp= mi_compare_text(cs, (uchar*) old_word->pos,old_word->len,
                             (uchar*) new_word->pos,new_word->len,0,0);
    cmp2= cmp ? 0 : (fabs(old_word->weight - new_word->weight) > 1.e-5);

    if (cmp < 0 || cmp2)
    {
      key_length=_ft_make_key(info,keynr,keybuf,old_word,pos);
      if ((error=_mi_ck_delete(info,keynr,(uchar*) keybuf,key_length)))
        goto err2;
    }
    if (cmp > 0 || cmp2)
    {
      key_length=_ft_make_key(info,keynr,keybuf,new_word,pos);
      if ((error=_mi_ck_write(info,keynr,(uchar*) keybuf,key_length)))
        goto err2;
    }
    if (cmp<=0) old_word++;
    if (cmp>=0) new_word++;
 }
 if (old_word->pos)
   error=_mi_ft_erase(info,keynr,keybuf,old_word,pos);
 else if (new_word->pos)
   error=_mi_ft_store(info,keynr,keybuf,new_word,pos);

err2:
    my_free((char*) newlist,MYF(0));
err1:
    my_free((char*) oldlist,MYF(0));
err0:
  DBUG_RETURN(error);
}


/* adds a document to the collection */

int _mi_ft_add(MI_INFO *info, uint keynr, byte *keybuf, const byte *record,
	       my_off_t pos)
{
  int error= -1;
  FT_WORD *wlist;
  DBUG_ENTER("_mi_ft_add");

  if ((wlist=_mi_ft_parserecord(info, keynr, record)))
  {
    error=_mi_ft_store(info,keynr,keybuf,wlist,pos);
    my_free((char*) wlist,MYF(0));
  }
  DBUG_RETURN(error);
}


/* removes a document from the collection */

int _mi_ft_del(MI_INFO *info, uint keynr, byte *keybuf, const byte *record,
	       my_off_t pos)
{
  int error= -1;
  FT_WORD *wlist;
  DBUG_ENTER("_mi_ft_del");
  DBUG_PRINT("enter",("keynr: %d",keynr));

  if ((wlist=_mi_ft_parserecord(info, keynr, record)))
  {
    error=_mi_ft_erase(info,keynr,keybuf,wlist,pos);
    my_free((char*) wlist,MYF(0));
  }
  DBUG_PRINT("exit",("Return: %d",error));
  DBUG_RETURN(error);
}

uint _ft_make_key(MI_INFO *info, uint keynr, byte *keybuf, FT_WORD *wptr,
		  my_off_t filepos)
{
  byte buf[HA_FT_MAXBYTELEN+16];
  DBUG_ENTER("_ft_make_key");

#if HA_FT_WTYPE == HA_KEYTYPE_FLOAT
  {
    float weight=(float) ((filepos==HA_OFFSET_ERROR) ? 0 : wptr->weight);
    mi_float4store(buf,weight);
  }
#else
#error
#endif

  int2store(buf+HA_FT_WLEN,wptr->len);
  memcpy(buf+HA_FT_WLEN+2,wptr->pos,wptr->len);
  DBUG_RETURN(_mi_make_key(info,keynr,(uchar*) keybuf,buf,filepos));
}


/*
  convert key value to ft2
*/

uint _mi_ft_convert_to_ft2(MI_INFO *info, uint keynr, uchar *key)
{
  my_off_t root;
  DYNAMIC_ARRAY *da=info->ft1_to_ft2;
  MI_KEYDEF *keyinfo=&info->s->ft2_keyinfo;
  uchar *key_ptr= (uchar*) dynamic_array_ptr(da, 0), *end;
  uint length, key_length;
  DBUG_ENTER("_mi_ft_convert_to_ft2");

  /* we'll generate one pageful at once, and insert the rest one-by-one */
  /* calculating the length of this page ...*/
  length=(keyinfo->block_length-2) / keyinfo->keylength;
  set_if_smaller(length, da->elements);
  length=length * keyinfo->keylength;

  get_key_full_length_rdonly(key_length, key);
  while (_mi_ck_delete(info, keynr, key, key_length) == 0)
  {
    /*
      nothing to do here.
      _mi_ck_delete() will populate info->ft1_to_ft2 with deleted keys
     */
  }

  /* creating pageful of keys */
  mi_putint(info->buff,length+2,0);
  memcpy(info->buff+2, key_ptr, length);
  info->buff_used=info->page_changed=1;           /* info->buff is used */
  if ((root= _mi_new(info,keyinfo,DFLT_INIT_HITS)) == HA_OFFSET_ERROR ||
      _mi_write_keypage(info,keyinfo,root,DFLT_INIT_HITS,info->buff))
    DBUG_RETURN(-1);

  /* inserting the rest of key values */
  end= (uchar*) dynamic_array_ptr(da, da->elements);
  for (key_ptr+=length; key_ptr < end; key_ptr+=keyinfo->keylength)
    if(_mi_ck_real_write_btree(info, keyinfo, key_ptr, 0, &root, SEARCH_SAME))
      DBUG_RETURN(-1);

  /* now, writing the word key entry */
  ft_intXstore(key+key_length, - (int) da->elements);
  _mi_dpointer(info, key+key_length+HA_FT_WLEN, root);

  DBUG_RETURN(_mi_ck_real_write_btree(info,
                                     info->s->keyinfo+keynr,
                                     key, 0,
                                     &info->s->state.key_root[keynr],
                                     SEARCH_SAME));
}

