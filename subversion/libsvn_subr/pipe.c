/*
 * pipe.c : utility functions for creating and communicating via
 *          interprocess pipes.
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include "svn_io.h"
#include "svn_pipe.h"

/*
 * Each endpoint of the pipe has an svn_pipe_t instance. 
 */
struct svn_pipe_t {
  apr_proc_t *proc;     /* the piped process */
  apr_file_t *read;     /* incoming data */
  apr_file_t *write;    /* outgoing data */
};

static svn_error_t *
procattr_creation_error(apr_status_t apr_err, apr_pool_t *pool)
{
  return svn_error_create(apr_err, 0, NULL, pool,
                          "couldn't create process attributes for pipe");
}

svn_error_t *
svn_pipe_open(svn_pipe_t **result,
              const char * const *argv,
              apr_pool_t *pool)
{
  svn_pipe_t *pipe;
  apr_procattr_t *attr;
  apr_status_t apr_err;

  pipe = apr_pcalloc(pool, sizeof(*pipe));


  apr_err = apr_procattr_create(&attr, pool);
  if (apr_err != APR_SUCCESS)
    return procattr_creation_error(apr_err, pool);

  apr_err = apr_procattr_io_set(attr,
                                APR_FULL_BLOCK,
                                APR_FULL_BLOCK,
                                APR_FULL_BLOCK);
  if (apr_err != APR_SUCCESS)
    return procattr_creation_error(apr_err, pool);

  apr_err = apr_procattr_cmdtype_set(attr, APR_PROGRAM_ENV);
  if (apr_err != APR_SUCCESS)
    return procattr_creation_error(apr_err, pool);

  apr_err = apr_procattr_detach_set(attr, 0);
  if (apr_err != APR_SUCCESS)
    return procattr_creation_error(apr_err, pool);

  pipe->proc = apr_pcalloc(pool, sizeof(*pipe->proc));
  apr_err = apr_proc_create(pipe->proc, argv[0], argv, NULL, attr, pool);
  if (apr_err != APR_SUCCESS)
    return svn_error_create(apr_err, 0, NULL, pool,
                            "couldn't create process for pipe");

  pipe->read = pipe->proc->out;
  pipe->write = pipe->proc->in;
  *result = pipe;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_pipe_endpoint(svn_pipe_t **result,
                  apr_file_t *input,
                  apr_file_t *output,
                  apr_pool_t *pool)
{
  svn_pipe_t *pipe;
  pipe = apr_pcalloc(pool, sizeof(*pipe));
  pipe->read = input;
  pipe->write = output;
  *result = pipe;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_pipe_close(svn_pipe_t *pipe)
{
  apr_file_close(pipe->read);
  apr_file_close(pipe->write);

  if (pipe->proc)
    apr_proc_wait(pipe->proc, NULL, NULL, APR_WAIT);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_pipe_send(svn_pipe_t *pipe,
              const char *data,
              apr_size_t length,
              apr_pool_t *pool)
{
  apr_status_t apr_err;
  char *header;

  header = apr_psprintf(pool, "%" APR_SSIZE_T_FMT ":", length);

  if (((apr_err = apr_file_write_full(pipe->write,
                                      header,
                                      strlen(header),
                                      NULL)) != APR_SUCCESS) ||
      ((apr_err = apr_file_write_full(pipe->write,
                                      data,
                                      length,
                                      NULL)) != APR_SUCCESS) ||
      ((apr_err = apr_file_flush(pipe->write)) != APR_SUCCESS))
    return svn_error_create(apr_err, 0, NULL, pool,
                            "ra_pipe: Couldn't send request");

  return SVN_NO_ERROR;
}

svn_error_t *
svn_pipe_receive(svn_pipe_t *pipe,
                 char **data,
                 apr_size_t *length,
                 apr_pool_t *pool)
{
  unsigned int frame_len = 0;
  unsigned int total = 0;
  apr_status_t apr_err;

  while( 1 )
    {
      char c;
      if (apr_file_getc(&c, pipe->read) != APR_SUCCESS)
          return svn_error_create(apr_err, 0, 0, pool,
                                  "pipe: could not read from peer");
      if (c == ':')
        break;
      frame_len = (frame_len * 10) + (c - '0');
    }

  *length = frame_len;
  *data = apr_pcalloc(pool, frame_len);

  while (total < frame_len)
    {
      char buf[BUFSIZ];
      apr_size_t len = sizeof(buf);

      apr_err = apr_file_read(pipe->read, buf, &len);
      if (apr_err)
        {
          if (!APR_STATUS_IS_EOF(apr_err))
            return svn_error_create(apr_err, 0, 0, pool,
                                    "pipe: could not read from peer");
          if ((total+len) < frame_len)
            return svn_error_create(apr_err, 0, 0, pool,
                                    "pipe: premature EOF in read");
        }

      memcpy(*data+total, buf, len);
      total += len;
    }

  return SVN_NO_ERROR;
}


  /* --------------------------------------------------------------
   * local variables:
   * eval: (load-file "../../tools/dev/svn-dev.el")
   * end:
   */
