dnl ==========================================================================
dnl Copyright (C) 2000 USAGI/WIDE Project.
dnl All rights reserved.
dnl 
dnl Redistribution and use in source and binary forms, with or without
dnl modification, are permitted provided that the following conditions
dnl are met:
dnl 1. Redistributions of source code must retain the above copyright
dnl    notice, this list of conditions and the following disclaimer.
dnl 2. Redistributions in binary form must reproduce the above copyright
dnl    notice, this list of conditions and the following disclaimer in the
dnl    documentation and/or other materials provided with the distribution.
dnl 3. Neither the name of the project nor the names of its contributors
dnl    may be used to endorse or promote products derived from this software
dnl    without specific prior written permission.
dnl 
dnl THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
dnl ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
dnl IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
dnl ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
dnl FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
dnl DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
dnl OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
dnl HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
dnl LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
dnl OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
dnl SUCH DAMAGE.
dnl ==========================================================================
dnl // $USAGI: in6_func_salen.m4,v 1.5 2000/10/30 15:19:46 yoshfuji Exp $
dnl ==================================================================================
dnl IN6_FUNC_SA_LEN(action-if-found[,action-if-not-found[,action-if-cross-compiling]])
dnl ==================================================================================
AC_DEFUN(IN6_FUNC_SA_LEN,[
AC_MSG_CHECKING([whether sys/socket.h has SA_LEN])
AC_CACHE_VAL(in6_cv_func_sa_len, [
AC_TRY_COMPILE([
#include <sys/types.h>
#include <sys/socket.h>
],[
struct sockaddr sa;
int main(){ int i = SA_LEN(&sa); return 0;}
],[	in6_cv_func_sa_len=yes
],[	in6_cv_func_sa_len=no
],[ 	in6_cv_func_sa_len=unknown
])])
if test "$in6_cv_func_sa_len" = "yes"; then
	AC_MSG_RESULT(yes)
	$1
elif test "$in6_cv_func_sa_len" = "no"; then
	AC_MSG_RESULT(no)
	$2
else
	AC_MSG_RESULT(unknown)
	$3
fi
])
