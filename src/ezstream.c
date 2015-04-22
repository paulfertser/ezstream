/*
 *  ezstream - source client for Icecast with external en-/decoder support
 *  Copyright (C) 2003, 2004, 2005, 2006  Ed Zaleski <oddsock@oddsock.org>
 *  Copyright (C) 2007, 2009, 2015        Moritz Grimm <mgrimm@mrsserver.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "compat.h"

#include "ezstream.h"

#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif

#include <shout/shout.h>

#include "cfg.h"
#include "configfile.h"
#include "log.h"
#include "metadata.h"
#include "playlist.h"
#include "util.h"
#include "xalloc.h"

#define STREAM_DONE	0
#define STREAM_CONT	1
#define STREAM_SKIP	2
#define STREAM_SERVERR	3
#define STREAM_UPDMDATA 4

int			 metadataFromProgram;

EZCONFIG		*pezConfig = NULL;
playlist_t		*playlist = NULL;
int			 playlistMode = 0;
unsigned int		 resource_errors = 0;

#ifdef HAVE_SIGNALS
const int		 ezstream_signals[] = {
	SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2
};

volatile sig_atomic_t	 rereadPlaylist = 0;
volatile sig_atomic_t	 rereadPlaylist_notify = 0;
volatile sig_atomic_t	 skipTrack = 0;
volatile sig_atomic_t	 queryMetadata = 0;
volatile sig_atomic_t	 quit = 0;
#else
int			 rereadPlaylist = 0;
int			 rereadPlaylist_notify = 0;
int			 skipTrack = 0;
int			 queryMetadata = 0;
int			 quit = 0;
#endif /* HAVE_SIGNALS */

typedef struct tag_ID3Tag {
	char	tag[3];
	char	trackName[30];
	char	artistName[30];
	char	albumName[30];
	char	year[3];
	char	comment[30];
	char	genre;
} ID3Tag;

int		urlParse(const char *, char **, unsigned short *, char **);
char *		shellQuote(const char *);
char *		replaceString(const char *, const char *, const char *);
char *		buildCommandString(const char *, const char *, metadata_t *);
char *		getMetadataString(const char *, metadata_t *);
metadata_t *	getMetadata(const char *);
int		setMetadata(shout_t *, metadata_t *, char **);
FILE *		openResource(shout_t *, const char *, int *, metadata_t **,
			     int *, long *);
int		reconnectServer(shout_t *, int);
const char *	getTimeString(long);
int		sendStream(shout_t *, FILE *, const char *, int, const char *,
			   struct timeval *);
int		streamFile(shout_t *, const char *);
int		streamPlaylist(shout_t *, const char *);
int		ez_shutdown(int);

#ifdef HAVE_SIGNALS
void		sig_handler(int);

# ifndef SIG_IGN
#  define SIG_IGN	 (void (*)(int))1
# endif /* !SIG_IGN */

void
sig_handler(int sig)
{
	switch (sig) {
	case SIGTERM:
	case SIGINT:
		quit = 1;
		break;
	case SIGHUP:
		rereadPlaylist = 1;
		rereadPlaylist_notify = 1;
		break;
	case SIGUSR1:
		skipTrack = 1;
		break;
	case SIGUSR2:
		queryMetadata = 1;
		break;
	default:
		break;
	}
}
#endif /* HAVE_SIGNALS */

int
urlParse(const char *url, char **hostname, unsigned short *port,
	 char **mountname)
{
	const char	*p1, *p2, *p3;
	char		 tmpPort[6] = "";
	size_t		 hostsiz, mountsiz;
	const char	*errstr;

	if (strncmp(url, "http://", strlen("http://")) != 0) {
		log_error("invalid <url>: not an HTTP address");
		return (0);
	}

	p1 = url + strlen("http://");
	p2 = strchr(p1, ':');
	if (p2 == NULL) {
		log_error("invalid <url>: missing port");
		return (0);
	}
	hostsiz = (p2 - p1) + 1;
	if (hostsiz <= 1) {
		log_error("invalid <url>: missing host");
		return (0);
	}
	*hostname = xmalloc(hostsiz);
	strlcpy(*hostname, p1, hostsiz);

	p2++;
	p3 = strchr(p2, '/');
	if (p3 == NULL || p3 - p2 >= (int)sizeof(tmpPort)) {
		log_error("invalid <url>: mountpoint missing, or port number too long");
		xfree(*hostname);
		return (0);
	}

	strlcpy(tmpPort, p2, (p3 - p2) + 1UL);
	*port = (unsigned short)strtonum(tmpPort, 1LL, (long long)USHRT_MAX, &errstr);
	if (errstr) {
		log_error("invalid <url>: port: %s is %s", tmpPort, errstr);
		xfree(*hostname);
		return (0);
	}

	mountsiz = strlen(p3) + 1;
	*mountname = xmalloc(mountsiz);
	strlcpy(*mountname, p3, mountsiz);

	return (1);
}

#define SHELLQUOTE_INLEN_MAX	8191UL

char *
shellQuote(const char *in)
{
	char		*out, *out_p;
	size_t		 out_len;
	const char	*in_p;

	out_len = (strlen(in) > SHELLQUOTE_INLEN_MAX)
	    ? SHELLQUOTE_INLEN_MAX
	    : strlen(in);
	out_len = out_len * 2 + 2;
	out = xcalloc(out_len + 1, sizeof(char));

	out_p = out;
	in_p = in;

	*out_p++ = '\'';
	out_len--;
	while (*in_p && out_len > 2) {
		switch (*in_p) {
		case '\'':
		case '\\':
			*out_p++ = '\\';
			out_len--;
			break;
		default:
			break;
		}
		*out_p++ = *in_p++;
		out_len--;
	}
	*out_p++ = '\'';

	return (out);
}

char *
replaceString(const char *source, const char *from, const char *to)
{
	char		*to_quoted, *dest;
	size_t		 dest_size;
	const char	*p1, *p2;

	to_quoted = shellQuote(to);
	dest_size = strlen(source) + strlen(to_quoted) + 1;
	dest = xcalloc(dest_size, sizeof(char));

	p1 = source;
	p2 = strstr(p1, from);
	if (p2 != NULL) {
		strncat(dest, p1, (size_t)(p2 - p1));
		strlcat(dest, to_quoted, dest_size);
		p1 = p2 + strlen(from);
	}
	strlcat(dest, p1, dest_size);

	xfree(to_quoted);

	return (dest);
}

char *
buildCommandString(const char *extension, const char *fileName,
		   metadata_t *mdata)
{
	char	*commandString = NULL;
	size_t	 commandStringLen = 0;
	char	*encoder = NULL;
	char	*decoder = NULL;
	char	*newDecoder = NULL;
	char	*newEncoder = NULL;
	char	*localTitle = UTF8toCHAR(metadata_get_title(mdata),
					 ICONV_REPLACE);
	char	*localArtist = UTF8toCHAR(metadata_get_artist(mdata),
					  ICONV_REPLACE);
	char	*localMetaString = UTF8toCHAR(metadata_get_string(mdata),
					      ICONV_REPLACE);

	decoder = xstrdup(getFormatDecoder(extension));
	if (strlen(decoder) == 0) {
		log_error("cannot decode: %s: unknown file extension %s",
		    fileName, extension);
		xfree(localTitle);
		xfree(localArtist);
		xfree(localMetaString);
		xfree(decoder);
		return (NULL);
	}
	newDecoder = replaceString(decoder, TRACK_PLACEHOLDER, fileName);
	if (strstr(decoder, ARTIST_PLACEHOLDER) != NULL) {
		char *tmpStr = replaceString(newDecoder, ARTIST_PLACEHOLDER,
		    localArtist);
		xfree(newDecoder);
		newDecoder = tmpStr;
	}
	if (strstr(decoder, TITLE_PLACEHOLDER) != NULL) {
		char *tmpStr = replaceString(newDecoder, TITLE_PLACEHOLDER,
		    localTitle);
		xfree(newDecoder);
		newDecoder = tmpStr;
	}
	/*
	 * if meta
	 *   if (prog && format)
	 *      metatoformat
	 *   else
	 *     if (!prog && title)
	 *       emptymeta
	 *     else
	 *       replacemeta
	 */
	if (strstr(decoder, METADATA_PLACEHOLDER) != NULL) {
		if (metadataFromProgram && pezConfig->metadataFormat != NULL) {
			char *mdataString = getMetadataString(pezConfig->metadataFormat, mdata);
			char *tmpStr = replaceString(newDecoder,
			    METADATA_PLACEHOLDER, mdataString);
			xfree(newDecoder);
			xfree(mdataString);
			newDecoder = tmpStr;
		} else {
			if (!metadataFromProgram && strstr(decoder, TITLE_PLACEHOLDER) != NULL) {
				char *tmpStr = replaceString(newDecoder,
				    METADATA_PLACEHOLDER, "");
				xfree(newDecoder);
				newDecoder = tmpStr;
			} else {
				char *tmpStr = replaceString(newDecoder,
				    METADATA_PLACEHOLDER, localMetaString);
				xfree(newDecoder);
				newDecoder = tmpStr;
			}
		}
	}

	encoder = xstrdup(getFormatEncoder(pezConfig->format));
	if (strlen(encoder) == 0) {
		log_notice("passing through%s%s data from the decoder",
		    (strcmp(pezConfig->format, THEORA_FORMAT) != 0) ? " (unsupported) " : " ",
		    pezConfig->format);
		commandStringLen = strlen(newDecoder) + 1;
		commandString = xcalloc(commandStringLen, sizeof(char));
		strlcpy(commandString, newDecoder, commandStringLen);
		xfree(localTitle);
		xfree(localArtist);
		xfree(localMetaString);
		xfree(decoder);
		xfree(encoder);
		xfree(newDecoder);
		return (commandString);
	}

	newEncoder = replaceString(encoder, ARTIST_PLACEHOLDER, localArtist);
	if (strstr(encoder, TITLE_PLACEHOLDER) != NULL) {
		char *tmpStr = replaceString(newEncoder, TITLE_PLACEHOLDER,
		    localTitle);
		xfree(newEncoder);
		newEncoder = tmpStr;
	}
	if (strstr(encoder, METADATA_PLACEHOLDER) != NULL) {
		if (metadataFromProgram && pezConfig->metadataFormat != NULL) {
			char *mdataString = getMetadataString(pezConfig->metadataFormat, mdata);
			char *tmpStr = replaceString(newEncoder,
			    METADATA_PLACEHOLDER, mdataString);
			xfree(newEncoder);
			xfree(mdataString);
			newEncoder = tmpStr;
		} else {
			if (!metadataFromProgram && strstr(encoder, TITLE_PLACEHOLDER) != NULL) {
				char *tmpStr = replaceString(newEncoder,
				    METADATA_PLACEHOLDER, "");
				xfree(newEncoder);
				newEncoder = tmpStr;
			} else {
				char *tmpStr = replaceString(newEncoder,
				    METADATA_PLACEHOLDER, localMetaString);
				xfree(newEncoder);
				newEncoder = tmpStr;
			}
		}
	}

	commandStringLen = strlen(newDecoder) + strlen(" | ") +
	    strlen(newEncoder) + 1;
	commandString = xcalloc(commandStringLen, sizeof(char));
	snprintf(commandString, commandStringLen, "%s | %s", newDecoder,
	    newEncoder);

	xfree(localTitle);
	xfree(localArtist);
	xfree(localMetaString);
	xfree(decoder);
	xfree(encoder);
	xfree(newDecoder);
	xfree(newEncoder);

	return (commandString);
}

char *
getMetadataString(const char *format, metadata_t *mdata)
{
	char	*tmp, *str;

	if (format == NULL)
		return (NULL);

	str = xstrdup(format);

	if (strstr(format, ARTIST_PLACEHOLDER) != NULL) {
		tmp = replaceString(str, ARTIST_PLACEHOLDER,
		    metadata_get_artist(mdata));
		xfree(str);
		str = tmp;
	}
	if (strstr(format, TITLE_PLACEHOLDER) != NULL) {
		tmp = replaceString(str, TITLE_PLACEHOLDER,
		    metadata_get_title(mdata));
		xfree(str);
		str = tmp;
	}
	if (strstr(format, STRING_PLACEHOLDER) != NULL) {
		tmp = replaceString(str, STRING_PLACEHOLDER,
		    metadata_get_string(mdata));
		xfree(str);
		str = tmp;
	}
	if (strstr(format, TRACK_PLACEHOLDER) != NULL) {
		tmp = replaceString(str, TRACK_PLACEHOLDER,
		    metadata_get_filename(mdata));
		xfree(str);
		str = tmp;
	}

	return (str);
}

metadata_t *
getMetadata(const char *fileName)
{
	metadata_t	*mdata;

	if (metadataFromProgram) {
		if ((mdata = metadata_program(fileName, cfg_normalize_strings())) == NULL)
			return (NULL);

		if (!metadata_program_update(mdata, METADATA_ALL)) {
			metadata_free(&mdata);
			return (NULL);
		}
	} else {
		if ((mdata = metadata_file(fileName, cfg_normalize_strings())) == NULL)
			return (NULL);

		if (!metadata_file_update(mdata)) {
			metadata_free(&mdata);
			return (NULL);
		}
	}

	return (mdata);
}

int
setMetadata(shout_t *shout, metadata_t *mdata, char **mdata_copy)
{
	shout_metadata_t	*shout_mdata = NULL;
	char			*songInfo;
	const char		*artist, *title;
	int			 ret = SHOUTERR_SUCCESS;

	if (cfg_no_metadata_updates())
		return (SHOUTERR_SUCCESS);

	if (mdata == NULL)
		return 1;

	if ((shout_mdata = shout_metadata_new()) == NULL) {
		log_syserr(ALERT, ENOMEM, "shout_metadata_new");
		exit(1);
	}

	artist = metadata_get_artist(mdata);
	title = metadata_get_title(mdata);

	/*
	 * We can do this, because we know how libshout works. This adds
	 * "charset=UTF-8" to the HTTP metadata update request and has the
	 * desired effect of letting newer-than-2.3.1 versions of Icecast know
	 * which encoding we're using.
	 */
	if (shout_metadata_add(shout_mdata, "charset", "UTF-8") != SHOUTERR_SUCCESS) {
		/* Assume SHOUTERR_MALLOC */
		log_syserr(ALERT, ENOMEM, "shout_metadata_add");
		exit(1);
	}

	if ((songInfo = getMetadataString(pezConfig->metadataFormat, mdata)) == NULL) {
		if (artist[0] == '\0' && title[0] == '\0')
			songInfo = xstrdup(metadata_get_string(mdata));
		else
			songInfo = metadata_assemble_string(mdata);
		if (artist[0] != '\0' && title[0] != '\0') {
			if (shout_metadata_add(shout_mdata, "artist", artist) != SHOUTERR_SUCCESS) {
				log_syserr(ALERT, ENOMEM,
				    "shout_metadata_add");
				exit(1);
			}
			if (shout_metadata_add(shout_mdata, "title", title) != SHOUTERR_SUCCESS) {
				log_syserr(ALERT, ENOMEM,
				    "shout_metadata_add");
				exit(1);
			}
		} else {
			if (shout_metadata_add(shout_mdata, "song", songInfo) != SHOUTERR_SUCCESS) {
				log_syserr(ALERT, ENOMEM,
				    "shout_metadata_add");
				exit(1);
			}
		}
	} else if (shout_metadata_add(shout_mdata, "song", songInfo) != SHOUTERR_SUCCESS) {
		log_syserr(ALERT, ENOMEM, "shout_metadata_add");
		exit(1);
	}

	if ((ret = shout_set_metadata(shout, shout_mdata)) != SHOUTERR_SUCCESS)
		log_warning("shout_set_metadata: %s", shout_get_error(shout));

	shout_metadata_free(shout_mdata);

	if (ret == SHOUTERR_SUCCESS) {
		if (mdata_copy != NULL && *mdata_copy == NULL)
			*mdata_copy = xstrdup(songInfo);
	}

	xfree(songInfo);
	return (ret);
}

FILE *
openResource(shout_t *shout, const char *fileName, int *popenFlag,
	     metadata_t **mdata_p, int *isStdin, long *songLen)
{
	FILE		*filep = NULL;
	char		 extension[25];
	char		*p = NULL;
	char		*pCommandString = NULL;
	metadata_t	*mdata;

	if (mdata_p != NULL)
		*mdata_p = NULL;
	if (songLen != NULL)
		*songLen = 0;

	if (strcmp(fileName, "stdin") == 0) {
		if (metadataFromProgram) {
			if ((mdata = getMetadata(pezConfig->metadataProgram)) == NULL)
				return (NULL);
			if (setMetadata(shout, mdata, NULL) != SHOUTERR_SUCCESS) {
				metadata_free(&mdata);
				return (NULL);
			}
			if (mdata_p != NULL)
				*mdata_p = mdata;
			else
				metadata_free(&mdata);
		}

		if (isStdin != NULL)
			*isStdin = 1;
		filep = stdin;
		return (filep);
	}

	if (isStdin != NULL)
		*isStdin = 0;

	extension[0] = '\0';
	p = strrchr(fileName, '.');
	if (p != NULL)
		strlcpy(extension, p, sizeof(extension));
	for (p = extension; *p != '\0'; p++)
		*p = tolower((int)*p);

	if (strlen(extension) == 0) {
		log_error("%s: cannot determine file type", fileName);
		return (filep);
	}

	if (metadataFromProgram) {
		if ((mdata = getMetadata(pezConfig->metadataProgram)) == NULL)
			return (NULL);
	} else {
		if ((mdata = getMetadata(fileName)) == NULL)
			return (NULL);
	}
	if (songLen != NULL)
		*songLen = metadata_get_length(mdata);

	*popenFlag = 0;
	if (pezConfig->reencode) {
		int	stderr_fd = -1;

		pCommandString = buildCommandString(extension, fileName, mdata);
		if (mdata_p != NULL)
			*mdata_p = mdata;
		else
			metadata_free(&mdata);
		log_info("running command: %s", pCommandString);

		if (cfg_quiet_stderr()) {
			int fd;

			stderr_fd = dup(fileno(stderr));
			if ((fd = open(_PATH_DEVNULL, O_RDWR, 0)) == -1) {
				log_alert("%s: %s", _PATH_DEVNULL,
				    strerror(errno));
				exit(1);
			}

			dup2(fd, fileno(stderr));
			if (fd > 2)
				close(fd);
		}

		fflush(NULL);
		errno = 0;
		if ((filep = popen(pCommandString, "r")) == NULL) {
			/* popen() does not set errno reliably ... */
			if (errno)
				log_error("execution error: %s: %s",
				    pCommandString, strerror(errno));
			else
				log_error("execution error: %s",
				    pCommandString);
		} else {
			*popenFlag = 1;
		}
		xfree(pCommandString);

		if (cfg_quiet_stderr())
			dup2(stderr_fd, fileno(stderr));

		if (stderr_fd > 2)
			close(stderr_fd);

		return (filep);
	}

	if (mdata_p != NULL)
		*mdata_p = mdata;
	else
		metadata_free(&mdata);

	if ((filep = fopen(fileName, "rb")) == NULL) {
		log_error("%s: %s", fileName, strerror(errno));
		return (NULL);
	}

	return (filep);
}

int
reconnectServer(shout_t *shout, int closeConn)
{
	unsigned int	i;
	int		close_conn = closeConn;

	log_warning("%s: connection lost", pezConfig->URL);

	i = 0;
	while (++i) {
		if (pezConfig->reconnectAttempts > 0)
			log_notice("reconnect: %s: attempt #%u/%u ...",
			    pezConfig->URL, i, pezConfig->reconnectAttempts);
		else
			log_notice("reconnect: %s: attempt #%u ...",
			    pezConfig->URL, i);

		if (close_conn == 0)
			close_conn = 1;
		else
			shout_close(shout);
		if (shout_open(shout) == SHOUTERR_SUCCESS) {
			log_notice("reconnect: %s: success",
			    pezConfig->URL);
			return (1);
		}

		log_warning("reconnect failed: %s: %s",
		    pezConfig->URL, shout_get_error(shout));

		if (pezConfig->reconnectAttempts > 0 &&
		    i >= pezConfig->reconnectAttempts)
			break;

		if (quit)
			return (0);
		else
			sleep(5);
	};

	log_warning("reconnect failed: giving up");
	return (0);
}

const char *
getTimeString(long seconds)
{
	static char	str[20];
	long		secs, mins, hours;

	if (seconds < 0)
		return (NULL);

	secs = seconds;
	hours = secs / 3600;
	secs %= 3600;
	mins = secs / 60;
	secs %= 60;

	snprintf(str, sizeof(str), "%ldh%02ldm%02lds", hours, mins, secs);
	return ((const char *)str);
}

int
sendStream(shout_t *shout, FILE *filepstream, const char *fileName,
	   int isStdin, const char *songLenStr, struct timeval *tv)
{
	unsigned char	 buff[4096];
	size_t		 bytes_read, total, oldTotal;
	int		 ret;
	double		 kbps = -1.0;
	struct timeval	 timeStamp, *startTime = tv;
	struct timeval	 callTime, currentTime;

	ez_gettimeofday((void *)&callTime);

	timeStamp.tv_sec = startTime->tv_sec;
	timeStamp.tv_usec = startTime->tv_usec;

	total = oldTotal = 0;
	ret = STREAM_DONE;
	while ((bytes_read = fread(buff, 1UL, sizeof(buff), filepstream)) > 0) {
		if (shout_get_connected(shout) != SHOUTERR_CONNECTED &&
		    reconnectServer(shout, 0) == 0) {
			ret = STREAM_SERVERR;
			break;
		}

		shout_sync(shout);

		if (shout_send(shout, buff, bytes_read) != SHOUTERR_SUCCESS) {
			log_error("shout_send: %s", shout_get_error(shout));
			if (reconnectServer(shout, 1))
				break;
			else {
				ret = STREAM_SERVERR;
				break;
			}
		}

		if (quit)
			break;
		if (rereadPlaylist_notify) {
			rereadPlaylist_notify = 0;
			if (!pezConfig->fileNameIsProgram)
				log_notice("HUP signal received: playlist re-read scheduled");
		}
		if (skipTrack) {
			skipTrack = 0;
			ret = STREAM_SKIP;
			break;
		}

		ez_gettimeofday((void *)&currentTime);

		if (queryMetadata ||
		    (pezConfig->metadataRefreshInterval != -1
			&& (currentTime.tv_sec - callTime.tv_sec
			    >= pezConfig->metadataRefreshInterval)
			)
		    ) {
			queryMetadata = 0;
			if (metadataFromProgram) {
				ret = STREAM_UPDMDATA;
				break;
			}
		}

		total += bytes_read;
		if (cfg_quiet_stderr() && cfg_verbosity()) {
			double	oldTime, newTime;

			if (!isStdin && playlistMode) {
				if (pezConfig->fileNameIsProgram) {
					char *tmp = xstrdup(pezConfig->fileName);
					printf("  [%s]", basename(tmp));
					xfree(tmp);
				} else
					printf("  [%4lu/%-4lu]",
					    playlist_get_position(playlist),
					    playlist_get_num_items(playlist));
			}

			oldTime = (double)timeStamp.tv_sec
			    + (double)timeStamp.tv_usec / 1000000.0;
			newTime = (double)currentTime.tv_sec
			    + (double)currentTime.tv_usec / 1000000.0;
			if (songLenStr == NULL)
				printf("  [ %s]",
				    getTimeString(currentTime.tv_sec -
					startTime->tv_sec));
			else
				printf("  [ %s/%s]",
				    getTimeString(currentTime.tv_sec -
					startTime->tv_sec),
				    songLenStr);
			if (newTime - oldTime >= 1.0) {
				kbps = (((double)(total - oldTotal)
				    / (newTime - oldTime)) * 8.0) / 1000.0;
				timeStamp.tv_sec = currentTime.tv_sec;
				timeStamp.tv_usec = currentTime.tv_usec;
				oldTotal = total;
			}
			if (kbps < 0)
				printf("                 ");
			else
				printf("  [%8.2f kbps]", kbps);

			printf("  \r");
			fflush(stdout);
		}
	}
	if (ferror(filepstream)) {
		if (errno == EINTR) {
			clearerr(filepstream);
			ret = STREAM_CONT;
		} else if (errno == EBADF && isStdin)
			log_notice("no (more) data available on standard input");
		else
			log_error("sendStream: %s: %s", fileName,
			    strerror(errno));
	}

	return (ret);
}

int
streamFile(shout_t *shout, const char *fileName)
{
	FILE		*filepstream = NULL;
	int		 popenFlag = 0;
	char		*songLenStr = NULL;
	int		 isStdin = 0;
	int		 ret, retval = 0;
	long		 songLen;
	metadata_t	*mdata;
	struct timeval	 startTime;

	if ((filepstream = openResource(shout, fileName, &popenFlag, &mdata, &isStdin, &songLen))
	    == NULL) {
		if (++resource_errors > 100) {
			log_error("too many errors; giving up");
			return (0);
		}
		/* Continue with next resource on failure: */
		return (1);
	}
	resource_errors = 0;

	if (mdata != NULL) {
		char	*tmp, *metaData;

		tmp = metadata_assemble_string(mdata);
		if ((metaData = UTF8toCHAR(tmp, ICONV_REPLACE)) == NULL)
			metaData = xstrdup("(unknown title)");
		xfree(tmp);
		log_notice("streaming: %s (%s)", metaData, fileName);
		xfree(metaData);

		/* MP3 streams are special, so set the metadata explicitly: */
		if (strcmp(pezConfig->format, MP3_FORMAT) == 0)
			setMetadata(shout, mdata, NULL);

		metadata_free(&mdata);
	} else if (isStdin)
		log_notice("streaming: standard input");

	if (songLen > 0)
		songLenStr = xstrdup(getTimeString(songLen));
	ez_gettimeofday((void *)&startTime);
	do {
		ret = sendStream(shout, filepstream, fileName, isStdin,
		    songLenStr, &startTime);
		if (quit)
			break;
		if (ret != STREAM_DONE) {
			if ((skipTrack && rereadPlaylist) ||
			    (skipTrack && queryMetadata)) {
				skipTrack = 0;
				ret = STREAM_CONT;
			}
			if (queryMetadata && rereadPlaylist) {
				queryMetadata = 0;
				ret = STREAM_CONT;
			}
			if (ret == STREAM_SKIP || skipTrack) {
				skipTrack = 0;
				if (!isStdin)
					log_notice("USR1 signal received: skipping current track");
				retval = 1;
				ret = STREAM_DONE;
			}
			if (ret == STREAM_UPDMDATA || queryMetadata) {
				queryMetadata = 0;
				if (cfg_no_metadata_updates())
					continue;
				if (metadataFromProgram) {
					char		*mdataStr = NULL;
					metadata_t	*prog_mdata;

					log_info("running metadata program: %s",
					    pezConfig->metadataProgram);
					if ((prog_mdata = getMetadata(pezConfig->metadataProgram)) == NULL) {
						retval = 0;
						ret = STREAM_DONE;
						continue;
					}
					if (setMetadata(shout, prog_mdata, &mdataStr) != SHOUTERR_SUCCESS) {
						retval = 0;
						ret = STREAM_DONE;
						continue;
					}
					metadata_free(&prog_mdata);
					log_info("new metadata: %s", mdataStr);
					xfree(mdataStr);
				}
			}
			if (ret == STREAM_SERVERR) {
				retval = 0;
				ret = STREAM_DONE;
			}
		} else
			retval = 1;
	} while (ret != STREAM_DONE);

	if (popenFlag)
		pclose(filepstream);
	else if (!isStdin)
		fclose(filepstream);

	if (songLenStr != NULL)
		xfree(songLenStr);

	return (retval);
}

int
streamPlaylist(shout_t *shout, const char *fileName)
{
	const char	*song;
	char		 lastSong[PATH_MAX];

	if (playlist == NULL) {
		if (pezConfig->fileNameIsProgram) {
			if ((playlist = playlist_program(fileName)) == NULL)
				return (0);
		} else {
			if ((playlist = playlist_read(fileName)) == NULL)
				return (0);
			if (playlist_get_num_items(playlist) == 0)
				log_notice("%s: playlist empty", fileName);
		}
	} else {
		/*
		 * XXX: This preserves traditional behavior, however,
		 *      rereading the playlist after each walkthrough seems a
		 *      bit more logical.
		 */
		playlist_rewind(playlist);
	}

	if (!pezConfig->fileNameIsProgram && pezConfig->shuffle)
		playlist_shuffle(playlist);

	while ((song = playlist_get_next(playlist)) != NULL) {
		strlcpy(lastSong, song, sizeof(lastSong));
		if (!streamFile(shout, song))
			return (0);
		if (quit)
			break;
		if (rereadPlaylist) {
			rereadPlaylist = rereadPlaylist_notify = 0;
			if (pezConfig->fileNameIsProgram)
				continue;
			log_notice("rereading playlist");
			if (!playlist_reread(&playlist))
				return (0);
			if (pezConfig->shuffle)
				playlist_shuffle(playlist);
			else {
				playlist_goto_entry(playlist, lastSong);
				playlist_skip_next(playlist);
			}
			continue;
		}
	}

	return (1);
}

int
ez_shutdown(int exitval)
{
	shout_shutdown();
	playlist_shutdown();
	freeConfig(pezConfig);
	xalloc_shutdown();
	log_exit();

	return (exitval);
}

int
main(int argc, char *argv[])
{
	int		 ret;

	const char	*configFile;
	const char	*playlistFile;
	char		*host = NULL;
	unsigned short	 port = 0;
	char		*mount = NULL;
	shout_t 	*shout;
	extern char	*optarg;
	extern int	 optind;
#ifdef HAVE_SIGNALS
	struct sigaction act;
	unsigned int	 i;
#endif
	ret = 1;
	if (0 > cfg_cmdline_parse(argc, argv, &ret))
		return (ret);
	log_init();

#ifdef XALLOC_DEBUG
	xalloc_initialize_debug(2, NULL);
#else
	xalloc_initialize();
#endif /* XALLOC_DEBUG */
	playlist_init();
	shout_init();

	pezConfig = getEZConfig();

	playlistFile = cfg_shuffle_file();
	if (playlistFile) {
		playlist_t	*pl;
		const char	*entry;

		if (0 == strcmp(playlistFile, "-"))
			pl = playlist_read(NULL);
		else
			pl = playlist_read(playlistFile);

		if (pl == NULL)
			return (ez_shutdown(1));

		playlist_shuffle(pl);
		while ((entry = playlist_get_next(pl)) != NULL)
			printf("%s\n", entry);

		playlist_free(&pl);

		return (ez_shutdown(0));
	}

	configFile = cfg_config_file();
	{
		/*
		 * Attempt to open configFile here for a more meaningful error
		 * message. Where possible, do it with stat() and check for
		 * safe config file permissions.
		 */
#ifdef HAVE_STAT
		struct stat	st;

		if (stat(configFile, &st) == -1) {
			log_error("%s: %s", configFile, strerror(errno));
			return (ez_shutdown(2));
		}
		if (cfg_verbosity() && (st.st_mode & (S_IRGRP | S_IROTH)))
			log_warning("%s: group and/or world readable",
			    configFile);
		if (st.st_mode & (S_IWGRP | S_IWOTH)) {
			log_error("%s: group and/or world writeable",
			    configFile);
			return (ez_shutdown(2));
		}
#else
		FILE		 *tmp;

		if ((tmp = fopen(configFile, "r")) == NULL) {
			log_error("%s: %s", configFile, strerror(errno));
			usage();
			return (ez_shutdown(2));
		}
		fclose(tmp);
#endif /* HAVE_STAT */
	}

	if (!parseConfig(configFile))
		return (ez_shutdown(2));

	if (pezConfig->URL == NULL) {
		log_error("%s: missing <url>", configFile);
		return (ez_shutdown(2));
	}
	if (!urlParse(pezConfig->URL, &host, &port, &mount)) {
		log_error("%s: <url>: must be of the form ``http://server:port/mountpoint''",
		    configFile);
		return (ez_shutdown(2));
	}
	if (pezConfig->password == NULL) {
		log_error("%s: <sourcepassword> missing", configFile);
		return (ez_shutdown(2));
	}
	if (pezConfig->fileName == NULL) {
		log_error("%s: <filename> missing", configFile);
		return (ez_shutdown(2));
	}
	if (pezConfig->format == NULL) {
		log_error("%s: <format> missing or unsupported value",
		    configFile);
	}

	if ((shout = stream_setup(host, port, mount)) == NULL)
		return (ez_shutdown(1));

	if (pezConfig->metadataProgram != NULL)
		metadataFromProgram = 1;
	else
		metadataFromProgram = 0;

#ifdef HAVE_SIGNALS
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_handler;
# ifdef SA_RESTART
	act.sa_flags = SA_RESTART;
# endif
	for (i = 0; i < sizeof(ezstream_signals) / sizeof(int); i++) {
		if (sigaction(ezstream_signals[i], &act, NULL) == -1) {
			log_syserr(ERROR, errno, "sigaction");
			return (ez_shutdown(1));
		}
	}
	/*
	 * Ignore SIGPIPE, which has been seen to give a long-running ezstream
	 * process trouble. EOF and/or EPIPE are also easier to handle.
	 */
	act.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &act, NULL) == -1) {
		log_syserr(ERROR, errno, "sigaction");
		return (ez_shutdown(1));
	}
#endif /* HAVE_SIGNALS */

	if (shout_open(shout) == SHOUTERR_SUCCESS) {
		int	cont;

		log_notice("connected: http://%s:%hu%s", host, port, mount);

		if (pezConfig->fileNameIsProgram ||
		    strrcasecmp(pezConfig->fileName, ".m3u") == 0 ||
		    strrcasecmp(pezConfig->fileName, ".txt") == 0)
			playlistMode = 1;
		else
			playlistMode = 0;

		do {
			if (playlistMode) {
				cont = streamPlaylist(shout, pezConfig->fileName);
			} else {
				cont = streamFile(shout, pezConfig->fileName);
			}
			if (quit)
				break;
			if (pezConfig->streamOnce)
				break;
		} while (cont);

		shout_close(shout);
	} else
		log_error("connection failed: http://%s:%hu%s: %s",
		    host, port, mount, shout_get_error(shout));

	if (quit) {
		if (cfg_quiet_stderr() && cfg_verbosity())
			printf("\r");
		log_notice("INT or TERM signal received");
	}

	log_info("exiting");

	xfree(host);
	xfree(mount);
	playlist_free(&playlist);

	return (ez_shutdown(0));
}
