/* This file is part of Hedgehog LISP.
 * Copyright (C) 2004, 2005 Oliotalo Ltd.
 * See file LICENSE.LGPL for pertinent licensing conditions.
 *
 * Author: Kenneth Oksanen <cessu@iki.fi>
 */

/* This file is #included into the C program generated by the the
   script interface-gen.pl. */

#include <stdio.h>
#include <string.h>

static void gen_int_flag(const char *prefix, const char *flag,
			 int value)
{
  /* printf("#ifdef %s\n", flag); */
  printf("(def-syntax %s%s %d)\n", prefix, flag, value);
  /* printf("#endif\n"); */
}

static void gen_int_flag_have(const char *prefix, const char *flag,
			      int value)
{
  /* printf("#ifdef %s\n", flag); */
  printf("#define %sHAVE-%s\n", prefix, flag);
  /* printf("#endif\n"); */
}

static void gen_struct(const char *prefix, const char *struct_name,
		       size_t size)
{
  int i;

  printf(";; Definitions for the size and field offsets, sizes and types for the C struct %s%s\n\n", prefix, struct_name);
  printf("(def-syntax %s%s \"", prefix, struct_name);
  for (i = 0; i < size; i++)
    printf("\\0");
  printf("\")\n");
}

#define OFFSETOF(s, f)  (((char *) &(s.f)) - ((char *) &(s)))

static void gen_field(const char *prefix, 
		      const char *struct_name,
		      const char *type,
		      const char *field,
		      int offset,
		      int size)
{
  printf("(def-syntax %s%s-%s-offset %d)\n",
	 prefix, struct_name, field, offset);
  printf("(def-syntax %s%s-%s-size %d)\n",
	 prefix, struct_name, field, size);
  printf("(def-syntax %s%s-%s-type \"%s\")\n",
	 prefix, struct_name, field, type);

  if (strcmp(type, "int ") == 0 
      || strcmp(type, "unsigned int ") == 0) {
    printf("(def-syntax (%s%s-get-%s ?x)\n  (c-get-int ?x %d))\n",
	   prefix, struct_name, field, offset);
    printf("(def-syntax (%s%s-set-%s ?x ?v)\n  (c-set-int ?x %d ?v))\n",
	   prefix, struct_name, field, offset);
  } else if (strcmp(type, "short ") == 0
	     || strcmp(type, "unsigned short ") == 0) {
    printf("(def-syntax (%s%s-get-%s ?x)\n  (c-get-short ?x %d))\n",
	   prefix, struct_name, field, offset);
    printf("(def-syntax (%s%s-set-%s ?x ?v)\n  (c-set-short ?x %d ?v))\n",
	   prefix, struct_name, field, offset);
  }
}


static void gen_byteorder(void)
{
  union {
    unsigned char c[4];
    unsigned int ui;
  } u4;

  u4.ui = 0xAABBCCDD;
  if (u4.c[0] == 0xAA
      && u4.c[1] == 0xBB
      && u4.c[2] == 0xCC
      && u4.c[3] == 0xDD)
    /* Most significant byte first. */
    printf("#define HH_MOST_SIGNIFICANT_BYTE_FIRST\n");
  else if (u4.c[0] == 0xDD
	     && u4.c[1] == 0xCC
	     && u4.c[2] == 0xBB
	     && u4.c[3] == 0xAA)
    printf("#define HH_LEAST_SIGNIFICANT_BYTE_FIRST\n");
  else
    printf(";; Unrecognized byte order.\n");
}
  

