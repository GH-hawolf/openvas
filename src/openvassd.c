/* OpenVAS
* $Id$
* Description: Runs the OpenVAS-scanner.
*
* Authors: - Renaud Deraison <deraison@nessus.org> (Original pre-fork develoment)
*          - Tim Brown <mailto:timb@openvas.org> (Initial fork)
*          - Laban Mwangi <mailto:labanm@openvas.org> (Renaming work)
*          - Tarik El-Yassem <mailto:tarik@openvas.org> (Headers section)
*
* Copyright:
* Portions Copyright (C) 2006 Software in the Public Interest, Inc.
* Based on work Copyright (C) 1998 - 2006 Tenable Network Security, Inc.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2,
* as published by the Free Software Foundation
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/**
 * @mainpage
 *
 * @section Introduction
 * @verbinclude README
 *
 * @section license License Information
 * @verbinclude COPYING
 */

/**
 * @file
 * OpenVAS Scanner main module, runs the scanner.
 */

#include <errno.h>
#include <string.h>    /* for strchr() */
#include <stdio.h>     /* for fprintf() */
#include <stdlib.h>    /* for atoi() */
#include <unistd.h>    /* for close() */
#include <errno.h>     /* for errno() */
#include <fcntl.h>     /* for open() */
#include <arpa/inet.h> /* for inet_aton */
#include <signal.h>    /* for SIGTERM */
#include <netdb.h>     /* for addrinfo */

#include <openvas/misc/bpf_share.h>  /* for bpf_server */
#include <openvas/nasl/nasl.h>
#include <openvas/misc/network.h>    /* for auth_printf */
#include <openvas/misc/plugutils.h>  /* for find_in_path */
#include <openvas/misc/system.h>     /* for estrdup */
#include <openvas/misc/rand.h>       /* for openvas_init_random */
#include <openvas/misc/openvas_proctitle.h>
#include <openvas/misc/openvas_logging.h>  /* for setup_legacy_log_handler */
#include <openvas/base/pidfile.h>    /* for pidfile_remove */
#include <openvas/misc/otp.h>        /* for OTP_20 */

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <glib.h>


#include "pluginload.h"
#include "preferences.h"
#include "comm.h"
#include "attack.h"
#include "sighand.h"
#include "log.h"
#include "processes.h"
#include "ntp_11.h"
#include "utils.h"
#include "pluginscheduler.h"
#include "pluginlaunch.h"

/**
 * Globals that should not be touched (used in utils module).
 */
int global_max_hosts = 15;
int global_max_checks = 10;
struct arglist *g_options = NULL;

pid_t bpf_server_pid;
pid_t nasl_server_pid;

int global_iana_socket;
struct arglist *global_plugins;
struct arglist *global_preferences;

static int restart = 0;

/**
 * SSL context may be kept once it is inited.
 */
static ovas_scanner_context_t ovas_scanner_ctx = NULL;

static void
dump_cfg_specs (struct arglist *prefs)
{
  while (prefs && prefs->next)
    {
      printf ("%s = %s\n", prefs->name, (char *) prefs->value);
      prefs = prefs->next;
    }
}

static void
arg_replace_value (struct arglist *arglist, char *name, int type, int length,
                   void *value)
{
  if (arg_get_type (arglist, name) < 0)
    arg_add_value (arglist, name, type, length, value);
  else
    arg_set_value (arglist, name, length, value);
}


static void
start_daemon_mode (void)
{
  char *s;
  int fd;


  /* do not block the listener port for subsequent scanners */
  close (global_iana_socket);

  /* become process group leader */
  if (setsid () < 0)
    {
      log_write ("Warning: Cannot set process group leader (%s)\n",
                 strerror (errno));
    }

  if ((fd = open ("/dev/tty", O_RDWR)) >= 0)
    close (fd);

  /* no input, anymore: provide an empty-file substitute */
  if ((fd = open ("/dev/null", O_RDONLY)) < 0)
    {
      log_write ("Cannot open /dev/null (%s) -- aborting\n", strerror (errno));
      exit (0);
    }

  dup2 (fd, 0);
  close (fd);

  /* provide a dump file to collect stdout and stderr */
  if ((s = arg_get_value (global_preferences, "dumpfile")) == 0)
    s = OPENVASSD_DEBUGMSG;
  /* setting "-" denotes terminal mode */
  if (strcmp (s, "-") == 0)
    return;

  fflush (stdout);
  fflush (stderr);

  if ((fd = open (s, O_WRONLY | O_CREAT | O_APPEND, 0600)) < 0)
    {
      log_write ("Cannot create a new dumpfile %s (%s)-- aborting\n", s,
                 strerror (errno));
      exit (2);
    }

  dup2 (fd, 1);
  dup2 (fd, 2);
  close (fd);
  setlinebuf (stdout);
  setlinebuf (stderr);
}


static void
end_daemon_mode (void)
{
  /* clean up all processes the process group */
  make_em_die (SIGTERM);
}

static void
set_globals_from_preferences (struct arglist *prefs)
{
  char *str;

  if ((str = arg_get_value (prefs, "max_hosts")) != NULL)
    {
      global_max_hosts = atoi (str);
      if (global_max_hosts <= 0)
        global_max_hosts = 15;
    }

  if ((str = arg_get_value (prefs, "max_checks")) != NULL)
    {
      global_max_checks = atoi (str);
      if (global_max_checks <= 0)
        global_max_checks = 10;
    }

  arg_free (global_preferences);
  global_preferences = prefs;
}

static void
sighup (int i)
{
  restart = 1;
}

/* Restarts the scanner by reloading the configuration. */
static void
reload_openvassd ()
{
  struct arglist *preferences = NULL, *plugins;
  char *config_file;

  log_write ("Reloading the scanner.\n");
  /* Ignore SIGHUP while reloading. */
  openvas_signal (SIGHUP, SIG_IGN);

  /* Reload config file. */
  config_file = arg_get_value (global_preferences, "config_file");
  preferences_init (config_file, &preferences);

  /* Reload the plugins */
  plugins = plugins_init (preferences);
  set_globals_from_preferences (preferences);
  plugins_free (global_plugins);
  global_plugins = plugins;

  log_write ("Finished reloading the scanner.\n");
  restart = 0;
  openvas_signal (SIGHUP, sighup);
}

int
check_client (char *dname)
{
  int success = 0;

  if (dname != NULL && *dname != '\0')
    {
      FILE *f;
      if ((f = fopen (OPENVAS_STATE_DIR "/dname", "r")) == NULL)
        perror (OPENVAS_STATE_DIR "/dname");
      else
        {
          char dnameref[512];

          while (! success
                 && fgets (dnameref, sizeof (dnameref) - 1, f) != NULL)
            {
              char *p;
              if ((p = strchr (dnameref, '\n')) != NULL)
                *p = '\0';
              if (strcmp (dname, dnameref) == 0)
                success = 1;
            }
          if (! success)
            log_write
              ("check_client: Bad DN\nGiven DN=%s\nLast tried DN=%s\n",
               dname, dnameref);
          fclose (f);
        }
    }

  return success;
}

static void
scanner_thread (struct arglist *globals)
{
  struct arglist *plugins = arg_get_value (globals, "plugins");
  struct arglist *prefs = arg_get_value (globals, "preferences");
  int soc = GPOINTER_TO_SIZE (arg_get_value (globals, "global_socket"));
  int family = GPOINTER_TO_SIZE (arg_get_value (globals, "family"));
  char *asciiaddr;
  int protocol_version;
  int opt = 1;
  void *addr = arg_get_value (globals, "client_address");
  struct sockaddr_in *saddr = NULL;
  struct sockaddr_in6 *s6addr = NULL;
  int nice_retval;

  char x509_dname[256];
  int soc2 = -1;

  asciiaddr = emalloc (INET6_ADDRSTRLEN);
  if (family == AF_INET)
    {
      saddr = (struct sockaddr_in *) addr;
      proctitle_set ("openvassd: Serving %s", inet_ntoa (saddr->sin_addr));
    }
  else
    {
      s6addr = (struct sockaddr_in6 *) addr;
      proctitle_set ("openvassd: Serving %s",
                     inet_ntop (AF_INET6, &s6addr->sin6_addr, asciiaddr,
                                sizeof (asciiaddr)));
    }
  efree (&asciiaddr);

  *x509_dname = '\0';

  /* Everyone runs with a nicelevel of 10 */
  if (preferences_benice (prefs))
    {
      errno = 0;
      nice_retval = nice (10);
      if (nice_retval == -1 && errno != 0)
        {
          log_write ("Unable to renice process: %d", errno);
        }
    }
  openvas_signal (SIGCHLD, sighand_chld);

  /* Close the scanner thread - it is useless for us now */
  close (global_iana_socket);

  soc2 = ovas_scanner_context_attach (ovas_scanner_ctx, soc);
  if (soc2 < 0)
    goto shutdown_and_exit;

  /* FIXME: The pre-gnutls code optionally printed information about
   * the peer's certificate at this point.
   */

  setsockopt (soc, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof (opt));
  /* arg_set_value *replaces* an existing value, but it shouldn't fail here */
  (void) arg_set_value (globals, "global_socket", -1, GSIZE_TO_POINTER (soc2));

  if (family == AF_INET)
    asciiaddr = estrdup (inet_ntoa (saddr->sin_addr));
  else
    {
      char *addrstr = emalloc (INET6_ADDRSTRLEN);
      asciiaddr =
        estrdup (inet_ntop
                 (AF_INET6, &s6addr->sin6_addr, addrstr, sizeof (addrstr)));
      efree (&addrstr);
    }

  protocol_version = comm_init (soc2);
  if (!protocol_version)
    {
      log_write ("New connection timeout -- closing the socket\n");
      close_stream_connection (soc);
      exit (0);
    }

  /* Get X.509 cert subject name */
  {
    gnutls_session_t *session;
    gnutls_x509_crt_t cert;
    unsigned int cert_list_size = 0;
    const gnutls_datum_t *cert_list;
    size_t x509_dname_size = sizeof (x509_dname);
    int ret;

    session = ovas_get_tlssession_from_connection (soc2);

    if (gnutls_certificate_type_get (*session) != GNUTLS_CRT_X509)
      {
        log_write ("Certificate is not an X.509 certificate.");
        goto shutdown_and_exit;
      }

    cert_list = gnutls_certificate_get_peers (*session, &cert_list_size);

    if (cert_list_size > 0)
      {
        gnutls_x509_crt_init (&cert);
        if ((ret =
             gnutls_x509_crt_import (cert, &cert_list[0],
                                     GNUTLS_X509_FMT_DER)) < 0)
          {
            log_write ("certificate decoding error: %s\n",
                       gnutls_strerror (ret));
            gnutls_x509_crt_deinit (cert);
            goto shutdown_and_exit;
          }
        if ((ret =
             gnutls_x509_crt_get_dn (cert, x509_dname, &x509_dname_size)) < 0)
          {
            log_write ("couldn't get subject from certificate: %s\n",
                       gnutls_strerror (ret));
            gnutls_x509_crt_deinit (cert);
            goto shutdown_and_exit;
          }
        gnutls_x509_crt_deinit (cert);
      }
  }

  if (! check_client (x509_dname))
    {
      auth_printf (globals, "Bad login attempt !\n");
      log_write ("bad login attempt from %s\n", asciiaddr);
      efree (&asciiaddr);
      goto shutdown_and_exit;
    }
  else
    {
      efree (&asciiaddr);

      arg_set_value (globals, "plugins", -1, plugins);

      // OTP 2.0 sends all plugins and other information at connect
      // OTP >=2.1 does not send these at connect
      if (protocol_version == OTP_20)
        {
          comm_send_nvt_info (globals);
          comm_send_preferences (globals);
          ntp_1x_send_dependencies (globals);
        }

      /* Become process group leader and the like ... */
      start_daemon_mode ();
    wait:
      comm_wait_order (globals);
      preferences_reset_cache ();
      ntp_1x_timestamp_scan_starts (globals);
      attack_network (globals);
      ntp_1x_timestamp_scan_ends (globals);
      comm_terminate (globals);
      if (arg_get_value (prefs, "ntp_keep_communication_alive"))
        {
          log_write ("Kept alive connection");
          goto wait;
        }
    }

shutdown_and_exit:
  if (soc2 >= 0)
    close_stream_connection (soc2);
  else
    {
      shutdown (soc, 2);
      close (soc);
    }

  /* Kill left overs */
  end_daemon_mode ();
  exit (0);
}

/*
 * Gives an OPENVAS_ENCAPS value matching an ssl version string.
 *
 * @param[in]   ssl_ver SSL version string.
 *
 * @return OPENVAS_ENCAPS value.
 */
static int
ssl_ver_to_encaps (const char *ssl_ver)
{
  if (ssl_ver == NULL)
    return OPENVAS_ENCAPS_TLSv1;

  if (strcasecmp (ssl_ver, "SSLv2") == 0)
    {
      fprintf (stderr, "SSL version 2 is not supported anymore!\n");
      return OPENVAS_ENCAPS_TLSv1;
    }
  else if (strcasecmp (ssl_ver, "SSLv3") == 0)
    return OPENVAS_ENCAPS_SSLv3;
  else if (strcasecmp (ssl_ver, "SSLv23") == 0)
    return OPENVAS_ENCAPS_SSLv23;
  else if (strcasecmp (ssl_ver, "TLSv1") == 0)
    return OPENVAS_ENCAPS_TLSv1;
  else
    {
      fprintf (stderr, "Unknown SSL version \"%s\"\n", ssl_ver);
      return OPENVAS_ENCAPS_TLSv1;
    }
}

static void
init_ssl_ctx ()
{
  if (openvas_SSL_init () < 0)
    {
      fprintf (stderr, "Could not initialize openvas SSL!\n");
      exit (1);
    }

  /* Only initialize ovas_scanner_ctx once */
  if (ovas_scanner_ctx == NULL)
    {
      int encaps;
      int force_pubkey_auth;
      char *cert, *key, *passwd, *ca_file, *ssl_ver;
      char *str;

      ssl_ver = preferences_get_string (global_preferences, "ssl_version");
      if (ssl_ver == NULL || *ssl_ver == '\0')
        ssl_ver = "TLSv1";
      encaps = ssl_ver_to_encaps (ssl_ver);

      ca_file = preferences_get_string (global_preferences, "ca_file");
      if (ca_file == NULL)
        {
          fprintf (stderr, "Missing ca_file - Did you run openvas-mkcert?\n");
          exit (1);
        }
      cert = preferences_get_string (global_preferences, "cert_file");
      if (cert == NULL)
        {
          fprintf (stderr, "Missing cert_file - Did you run openvas-mkcert?\n");
          exit (1);
        }
      key = preferences_get_string (global_preferences, "key_file");
      if (key == NULL)
        {
          fprintf (stderr, "Missing key_file - Did you run openvas-mkcert?\n");
          exit (1);
        }

      passwd = preferences_get_string (global_preferences, "pem_password");
      str = arg_get_value (global_preferences, "force_pubkey_auth");
      force_pubkey_auth = str != NULL && strcmp (str, "no") != 0;
      ovas_scanner_ctx =
        ovas_scanner_context_new (encaps, cert, key, passwd, ca_file,
                                  force_pubkey_auth);
      if (!ovas_scanner_ctx)
        {
          fprintf (stderr, "Could not create ovas_scanner_ctx\n");
          exit (1);
        }
    }
}

static void
main_loop ()
{
  struct addrinfo *ai = arg_get_value (g_options, "addr");

  /* catch dead children */
  openvas_signal (SIGCHLD, sighand_chld);

#if DEBUG_SSL > 1
  fprintf (stderr, "**** in main_loop ****\n");
#endif

  openvas_init_random ();

  init_ssl_ctx ();
  log_write ("openvassd %s started\n", OPENVASSD_VERSION);
  proctitle_set ("openvassd: Waiting for incoming connections");
  for (;;)
    {
      int soc;
      int family;
      unsigned int lg_address;
      char asciiaddr[INET6_ADDRSTRLEN];
      struct sockaddr_in address;
      struct sockaddr_in6 address6;
      struct sockaddr_in6 *p_addr;
      struct sockaddr_in *saddr;

      struct arglist *globals;
      struct arglist *my_plugins, *my_preferences;

      if (restart != 0)
        {
          proctitle_set ("openvassd: Reloading");
          reload_openvassd ();
          proctitle_set ("openvassd: Waiting for incoming connections");
        }

      wait_for_children1 ();
      if (ai->ai_family == AF_INET)
        {
          lg_address = sizeof (struct sockaddr_in);
          soc =
            accept (global_iana_socket, (struct sockaddr *) (&address),
                    &lg_address);
        }
      else
        {
          lg_address = sizeof (struct sockaddr_in6);
          soc =
            accept (global_iana_socket, (struct sockaddr *) (&address6),
                    &lg_address);
        }
      if (soc == -1)
        continue;

      family = ai->ai_family;
      if (family == AF_INET)
        {
          saddr = (struct sockaddr_in *) &address;
          if (inet_ntop (AF_INET, &saddr->sin_addr, asciiaddr,
                         sizeof (asciiaddr)) == NULL)
            exit (0);
        }
      else if (inet_ntop (AF_INET6, &address6.sin6_addr, asciiaddr,
                          sizeof (asciiaddr)) == NULL)
        exit (0);

/* FIXME: Find out whether following comment is still valid.
          Especially, I do not see where the arglists are duplicated with
          arg_add_value and TYPE != ARG_STRUCT, just pointers are copied, imho.
 */
      /* Duplicate everything so that the threads don't share the
       * same variables.
       *
       * Useless when fork is used, necessary for the pthreads.
       *
       * MA: you cannot share an open SSL connection through fork/multithread
       * The SSL connection shall be open _after_ the fork */
      globals = emalloc (sizeof (struct arglist));
      arg_add_value (globals, "global_socket", ARG_INT, -1,
                     GSIZE_TO_POINTER (soc));

      my_plugins = global_plugins;
      arg_add_value (globals, "plugins", ARG_ARGLIST, -1, my_plugins);

      my_preferences = global_preferences;
      arg_add_value (globals, "preferences", ARG_ARGLIST, -1, my_preferences);

      p_addr = emalloc (sizeof (struct sockaddr_in6));
      if (ai->ai_family == AF_INET)
        memcpy (p_addr, &address, sizeof (address));
      else
        memcpy (p_addr, &address6, sizeof (address6));
      arg_add_value (globals, "client_address", ARG_PTR, -1, p_addr);
      arg_add_value (globals, "family", ARG_INT, -1, GSIZE_TO_POINTER (family));
      free (p_addr);

      /* we do not want to create an io thread, yet so the last argument is -1 */
      if (create_process ((process_func_t) scanner_thread, globals) < 0)
        {
          log_write ("Could not fork - client won't be served");
          sleep (2);
        }
      close (soc);
      arg_free (globals);
    }
}

/**
 * Initialization of the network :
 * we setup the socket that will listen for incoming connections on port \<port\>
 * on address \<addr\>
 *
 * @param port Port on which to listen.
 * @param[out] sock Socket to be initialized.
 * @param addr Adress.
 *
 * @return 0 on success. Exit(1)s on failure.
 */
static int
init_network (int port, int *sock, struct addrinfo addr)
{
  int option = 1;

  if (addr.ai_family == AF_INET)
    ((struct sockaddr_in *) (addr.ai_addr))->sin_port = htons (port);
  else
    ((struct sockaddr_in6 *) (addr.ai_addr))->sin6_port = htons (port);
  if ((*sock = socket (addr.ai_family, SOCK_STREAM, 0)) == -1)
    {
      int ec = errno;
      log_write ("socket(AF_INET): %s (errno = %d)\n", strerror (ec), ec);
      fprintf (stderr, "socket() failed : %s\n", strerror (ec));
      exit (1);
    }

  setsockopt (*sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof (int));
  if (bind (*sock, (struct sockaddr *) (addr.ai_addr), addr.ai_addrlen) == -1)
    {
      fprintf (stderr, "bind() failed : %s\n", strerror (errno));
      exit (1);
    }

  if (listen (*sock, 10) == -1)
    {
      fprintf (stderr, "listen() failed : %s\n", strerror (errno));
      shutdown (*sock, 2);
      close (*sock);
      exit (1);
    }
  return (0);
}

static void
init_plugins (struct arglist *options)
{
  struct arglist *preferences, *plugins;

  preferences = arg_get_value (options, "preferences");
  plugins = plugins_init (preferences);

  arg_replace_value (options, "plugins", ARG_ARGLIST, -1, plugins);
  plugins_free (global_plugins);
  global_plugins = plugins;
}

/**
 * @brief Initialize everything.
 *
 * @param stop_early 0: do some initialization, 1: no initialization.
 */
static int
init_openvassd (struct arglist *options, int first_pass, int stop_early,
                int dont_fork)
{
  int isck = -1;
  struct arglist *preferences = NULL;
  int scanner_port = GPOINTER_TO_SIZE (arg_get_value (options, "scanner_port"));
  char *config_file = arg_get_value (options, "config_file");
  struct addrinfo *addr = arg_get_value (options, "addr");

  preferences_init (config_file, &preferences);

  log_init (arg_get_value (preferences, "logfile"));
  if (dont_fork == FALSE)
    setup_legacy_log_handler (log_vwrite);

  if (!stop_early)
    {
      if (first_pass != 0)
        init_network (scanner_port, &isck, *addr);
    }

  if (first_pass && !stop_early)
    {
      openvas_signal (SIGSEGV, sighandler);
      openvas_signal (SIGCHLD, sighand_chld);
      openvas_signal (SIGTERM, sighandler);
      openvas_signal (SIGINT, sighandler);
      openvas_signal (SIGHUP, sighup);
      openvas_signal (SIGUSR1, sighandler);     /* openvassd dies, not its sons */
      openvas_signal (SIGPIPE, SIG_IGN);
    }

  arg_replace_value (options, "isck", ARG_INT, sizeof (gpointer),
                     GSIZE_TO_POINTER (isck));
  arg_replace_value (options, "preferences", ARG_ARGLIST, -1, preferences);
  set_globals_from_preferences (preferences);

  return 0;
}

/**
 * @brief openvassd.
 * @param argc Argument count.
 * @param argv Argument vector.
 */
int
main (int argc, char *argv[])
{
  int exit_early = 0;
  int scanner_port = 9391;
  char *myself;
  struct arglist *options = emalloc (sizeof (struct arglist));
  struct addrinfo *mysaddr;
  struct addrinfo hints;
  struct addrinfo ai;
  struct sockaddr_in saddr;
  struct sockaddr_in6 s6addr;

  proctitle_init (argc, argv);

  if ((myself = strrchr (*argv, '/')) == 0)
    myself = *argv;
  else
    myself++;

  static gboolean display_version = FALSE;
  static gboolean dont_fork = FALSE;
  static gchar *address = NULL;
  static gchar *port = NULL;
  static gchar *config_file = NULL;
  static gboolean print_specs = FALSE;
  static gboolean print_sysconfdir = FALSE;
  static gboolean only_cache = FALSE;
  GError *error = NULL;
  GOptionContext *option_context;
  static GOptionEntry entries[] = {
    {"version", 'V', 0, G_OPTION_ARG_NONE, &display_version,
     "Display version information", NULL},
    {"foreground", 'f', 0, G_OPTION_ARG_NONE, &dont_fork,
     "Do not run in daemon mode but stay in foreground", NULL},
    {"listen", 'a', 0, G_OPTION_ARG_STRING, &address,
     "Listen on <address>", "<address>"},
    {"port", 'p', 0, G_OPTION_ARG_STRING, &port,
     "Use port number <number>", "<number>"},
    {"config-file", 'c', 0, G_OPTION_ARG_FILENAME, &config_file,
     "Configuration file", "<.rcfile>"},
    {"cfg-specs", 's', 0, G_OPTION_ARG_NONE, &print_specs,
     "Print configuration settings", NULL},
    {"sysconfdir", 'y', 0, G_OPTION_ARG_NONE, &print_sysconfdir,
     "Print system configuration directory (set at compile time)", NULL},
    {"only-cache", 'C', 0, G_OPTION_ARG_NONE, &only_cache,
     "Exit once the NVT cache has been initialized or updated", NULL},
    {NULL}
  };

  option_context =
    g_option_context_new ("- Scanner of the Open Vulnerability Assessment System");
  g_option_context_add_main_entries (option_context, entries, NULL);
  if (!g_option_context_parse (option_context, &argc, &argv, &error))
    {
      g_print ("%s\n\n", error->message);
      exit (0);
    }
  g_option_context_free (option_context);

  if (print_sysconfdir)
    {
      g_print ("%s\n", SYSCONFDIR);
      exit (0);
    }

  /* Switch to UTC so that OTP times are always in UTC. */
  if (setenv ("TZ", "utc 0", 1) == -1)
    {
      g_print ("%s\n\n", strerror (errno));
      exit (0);
    }
  tzset ();

  if (print_specs)
    exit_early = 2;           /* no cipher initialization */

  if (address != NULL)
    {
      memset (&hints, 0, sizeof (hints));
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = IPPROTO_TCP;
      hints.ai_flags = AI_NUMERICHOST;
      if (getaddrinfo (address, NULL, &hints, &mysaddr))
        {
          printf ("Invalid IP address.\n");
          printf ("Please use %s --help for more information.\n", myself);
          exit (0);
        }
      /* deep copy */
      ai.ai_family = mysaddr->ai_family;
      if (ai.ai_family == AF_INET)
        {
          memcpy (&saddr, mysaddr->ai_addr, mysaddr->ai_addrlen);
          ai.ai_addr = (struct sockaddr *) &saddr;
        }
      else
        {
          memcpy (&s6addr, mysaddr->ai_addr, mysaddr->ai_addrlen);
          ai.ai_addr = (struct sockaddr *) &s6addr;
        }
      ai.ai_family = mysaddr->ai_family;
      ai.ai_protocol = mysaddr->ai_protocol;
      ai.ai_socktype = mysaddr->ai_socktype;
      ai.ai_addrlen = mysaddr->ai_addrlen;
      freeaddrinfo (mysaddr);
    }
  else
    {
      /* Default to IPv4 */
      /*Warning: Not filling all the fields */
      saddr.sin_addr.s_addr = INADDR_ANY;
      saddr.sin_family = ai.ai_family = AF_INET;
      ai.ai_addrlen = sizeof (saddr);
      ai.ai_addr = (struct sockaddr *) &saddr;
    }

  if (port != NULL)
    {
      scanner_port = atoi (port);
      if ((scanner_port <= 0) || (scanner_port >= 65536))
        {
          printf ("Invalid port specification.\n");
          printf ("Please use %s --help for more information.\n", myself);
          exit (1);
        }
    }

  if (display_version)
    {
      printf ("OpenVAS Scanner %s\n", OPENVASSD_VERSION);
      printf
        ("Nessus origin: (C) 2004 Renaud Deraison <deraison@nessus.org>\n");
      printf
        ("Most new code since OpenVAS: (C) 2013 Greenbone Networks GmbH\n");
      printf ("License GPLv2: GNU GPL version 2\n");
      printf
        ("This is free software: you are free to change and redistribute it.\n"
         "There is NO WARRANTY, to the extent permitted by law.\n\n");
      exit (0);
    }

  if (config_file != NULL)
    arg_add_value (options, "acc_hint", ARG_INT, sizeof (int), (void *) 1);

  if (exit_early == 0)
    bpf_server_pid = bpf_server ();

  if (!config_file)
    {
      config_file = emalloc (strlen (OPENVASSD_CONF) + 1);
      strncpy (config_file, OPENVASSD_CONF, strlen (OPENVASSD_CONF));
    }

  arg_add_value (options, "scanner_port", ARG_INT, sizeof (gpointer),
                 GSIZE_TO_POINTER (scanner_port));
  arg_add_value (options, "config_file", ARG_STRING, strlen (config_file),
                 config_file);
  arg_add_value (options, "addr", ARG_PTR, -1, &ai);

  init_openvassd (options, 1, exit_early, dont_fork);
  g_options = options;
  global_iana_socket = GPOINTER_TO_SIZE (arg_get_value (options, "isck"));
  global_plugins = arg_get_value (options, "plugins");

  /* special treatment */
  if (print_specs)
    dump_cfg_specs (global_preferences);
  if (exit_early)
    exit (0);
  if (only_cache)
    exit (0);

  // Daemon mode:
  if (dont_fork == FALSE)
    {
      /* Close stdin, stdout and stderr */
      int i = open ("/dev/null", O_RDONLY, 0640);
      if (dup2 (i, STDIN_FILENO) != STDIN_FILENO)
        fprintf (stderr, "Could not redirect stdin to /dev/null: %s\n",
                 strerror (errno));
      if (dup2 (i, STDOUT_FILENO) != STDOUT_FILENO)
        fprintf (stderr, "Could not redirect stdout to /dev/null: %s\n",
                 strerror (errno));
      if (dup2 (i, STDERR_FILENO) != STDERR_FILENO)
        fprintf (stderr, "Could not redirect stderr to /dev/null: %s\n",
                 strerror (errno));
      close (i);
      if (!fork ())
        {
          setsid ();
          pidfile_create ("openvassd");
          init_plugins (options);
          main_loop ();
        }
    }
  else
    {
      pidfile_create ("openvassd");
      init_plugins (options);
      main_loop ();
    }
  exit (0);
}
