/*
 *  load_editor.c: The svn_delta_editor_t editor used by svnrdump to
 *  load revisions.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include "svn_cmdline.h"
#include "svn_pools.h"
#include "svn_delta.h"
#include "svn_repos.h"
#include "svn_props.h"
#include "svn_path.h"
#include "svn_ra.h"
#include "svn_io.h"
#include "svn_private_config.h"
#include "private/svn_repos_private.h"
#include "private/svn_ra_private.h"

#include "load_editor.h"

#define SVNRDUMP_PROP_LOCK SVN_PROP_PREFIX "rdump-lock"

#if 0
#define LDR_DBG(x) SVN_DBG(x)
#else
#define LDR_DBG(x) while(0)
#endif

static svn_error_t *
commit_callback(const svn_commit_info_t *commit_info,
                void *baton,
                apr_pool_t *pool)
{
  /* ### Don't print directly; generate a notification. */
  SVN_ERR(svn_cmdline_printf(pool, "* Loaded revision %ld.\n",
                             commit_info->revision));
  return SVN_NO_ERROR;
}

/* Implements `svn_ra__lock_retry_func_t'. */
static svn_error_t *
lock_retry_func(void *baton,
                const svn_string_t *reposlocktoken,
                apr_pool_t *pool)
{
  return svn_cmdline_printf(pool,
                            _("Failed to get lock on destination "
                              "repos, currently held by '%s'\n"),
                            reposlocktoken->data);
}

/* Acquire a lock (of sorts) on the repository associated with the
 * given RA SESSION. This lock is just a revprop change attempt in a
 * time-delay loop. This function is duplicated by svnsync in main.c.
 *
 * ### TODO: Make this function more generic and
 * expose it through a header for use by other Subversion
 * applications to avoid duplication.
 */
static svn_error_t *
get_lock(const svn_string_t **lock_string_p,
         svn_ra_session_t *session,
         svn_cancel_func_t cancel_func,
         void *cancel_baton,
         apr_pool_t *pool)
{
  svn_boolean_t be_atomic;

  SVN_ERR(svn_ra_has_capability(session, &be_atomic,
                                SVN_RA_CAPABILITY_ATOMIC_REVPROPS,
                                pool));
  if (! be_atomic)
    {
      /* Pre-1.7 servers can't lock without a race condition.  (Issue #3546) */
      svn_error_t *err =
        svn_error_create(SVN_ERR_UNSUPPORTED_FEATURE, NULL,
                         _("Target server does not support atomic revision "
                           "property edits; consider upgrading it to 1.7."));
      svn_handle_warning2(stderr, err, "svnrdump: ");
      svn_error_clear(err);
    }

  return svn_ra__get_operational_lock(lock_string_p, NULL, session,
                                      SVNRDUMP_PROP_LOCK, FALSE,
                                      10 /* retries */, lock_retry_func, NULL,
                                      cancel_func, cancel_baton, pool);
}

static svn_error_t *
new_revision_record(void **revision_baton,
                    apr_hash_t *headers,
                    void *parse_baton,
                    apr_pool_t *pool)
{
  struct revision_baton *rb;
  struct parse_baton *pb;
  apr_hash_index_t *hi;

  rb = apr_pcalloc(pool, sizeof(*rb));
  pb = parse_baton;
  rb->pool = svn_pool_create(pool);
  rb->pb = pb;

  for (hi = apr_hash_first(pool, headers); hi; hi = apr_hash_next(hi))
    {
      const char *hname = svn__apr_hash_index_key(hi);
      const char *hval = svn__apr_hash_index_val(hi);

      if (strcmp(hname, SVN_REPOS_DUMPFILE_REVISION_NUMBER) == 0)
        rb->rev = atoi(hval);
    }

  /* Set the commit_editor/ commit_edit_baton to NULL and wait for
     them to be created in new_node_record */
  rb->pb->commit_editor = NULL;
  rb->pb->commit_edit_baton = NULL;
  rb->revprop_table = apr_hash_make(rb->pool);

  *revision_baton = rb;
  return SVN_NO_ERROR;
}

static svn_error_t *
uuid_record(const char *uuid,
            void *parse_baton,
            apr_pool_t *pool)
{
  struct parse_baton *pb;
  pb = parse_baton;
  pb->uuid = apr_pstrdup(pool, uuid);
  return SVN_NO_ERROR;
}

static svn_error_t *
new_node_record(void **node_baton,
                apr_hash_t *headers,
                void *revision_baton,
                apr_pool_t *pool)
{
  struct revision_baton *rb = revision_baton;
  const struct svn_delta_editor_t *commit_editor = rb->pb->commit_editor;
  void *commit_edit_baton = rb->pb->commit_edit_baton;
  struct node_baton *nb;
  struct directory_baton *child_db;
  apr_hash_index_t *hi;
  void *child_baton;
  char *relpath_compose;
  const char *nb_dirname;

  nb = apr_pcalloc(rb->pool, sizeof(*nb));
  nb->rb = rb;

  nb->copyfrom_path = NULL;
  nb->copyfrom_rev = SVN_INVALID_REVNUM;

  /* If the creation of commit_editor is pending, create it now and
     open_root on it; also create a top-level directory baton. */

  if (!commit_editor)
    {
      /* The revprop_table should have been filled in with important
         information like svn:log in set_revision_property. We can now
         use it all this information to create our commit_editor. But
         first, clear revprops that we aren't allowed to set with the
         commit_editor. We'll set them separately using the RA API
         after closing the editor (see close_revision). */

      apr_hash_set(rb->revprop_table, SVN_PROP_REVISION_AUTHOR,
                   APR_HASH_KEY_STRING, NULL);
      apr_hash_set(rb->revprop_table, SVN_PROP_REVISION_DATE,
                   APR_HASH_KEY_STRING, NULL);

      SVN_ERR(svn_ra_get_commit_editor3(rb->pb->session, &commit_editor,
                                        &commit_edit_baton, rb->revprop_table,
                                        commit_callback, NULL, NULL, FALSE,
                                        rb->pool));

      rb->pb->commit_editor = commit_editor;
      rb->pb->commit_edit_baton = commit_edit_baton;

      SVN_ERR(commit_editor->open_root(commit_edit_baton, rb->rev - 1,
                                       rb->pool, &child_baton));

      LDR_DBG(("Opened root %p\n", child_baton));

      /* child_db corresponds to the root directory baton here */
      child_db = apr_pcalloc(rb->pool, sizeof(*child_db));
      child_db->baton = child_baton;
      child_db->depth = 0;
      child_db->relpath = "";
      child_db->parent = NULL;
      rb->db = child_db;
    }

  for (hi = apr_hash_first(rb->pool, headers); hi; hi = apr_hash_next(hi))
    {
      const char *hname = svn__apr_hash_index_key(hi);
      const char *hval = svn__apr_hash_index_val(hi);

      /* Parse the different kinds of headers we can encounter and
         stuff them into the node_baton for writing later */
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_PATH) == 0)
        nb->path = apr_pstrdup(rb->pool, hval);
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_KIND) == 0)
        nb->kind = strcmp(hval, "file") == 0 ? svn_node_file : svn_node_dir;
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_ACTION) == 0)
        {
          if (strcmp(hval, "add") == 0)
            nb->action = svn_node_action_add;
          if (strcmp(hval, "change") == 0)
            nb->action = svn_node_action_change;
          if (strcmp(hval, "delete") == 0)
            nb->action = svn_node_action_delete;
          if (strcmp(hval, "replace") == 0)
            nb->action = svn_node_action_replace;
        }
      if (strcmp(hname, SVN_REPOS_DUMPFILE_TEXT_DELTA_BASE_MD5) == 0)
        nb->base_checksum = apr_pstrdup(rb->pool, hval);
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_COPYFROM_REV) == 0)
        nb->copyfrom_rev = atoi(hval);
      if (strcmp(hname, SVN_REPOS_DUMPFILE_NODE_COPYFROM_PATH) == 0)
        nb->copyfrom_path =
          svn_path_url_add_component2(rb->pb->root_url,
                                      apr_pstrdup(rb->pool, hval),
                                      rb->pool);
    }

  nb_dirname = svn_relpath_dirname(nb->path, pool);
  if (svn_path_compare_paths(nb_dirname,
                             rb->db->relpath) != 0)
    {
      char *ancestor_path;
      apr_size_t residual_close_count;
      apr_array_header_t *residual_open_path;
      int i;

      /* Before attempting to handle the action, call open_directory
         for all the path components and set the directory baton
         accordingly */
      ancestor_path =
        svn_relpath_get_longest_ancestor(nb_dirname,
                                         rb->db->relpath, pool);
      residual_close_count =
        svn_path_component_count(svn_relpath_skip_ancestor(ancestor_path,
                                                           rb->db->relpath));
      residual_open_path =
        svn_path_decompose(svn_relpath_skip_ancestor(ancestor_path,
                                                     nb_dirname), pool);

      /* First close all as many directories as there are after
         skip_ancestor, and then open fresh directories */
      for (i = 0; i < residual_close_count; i ++)
        {
          /* Don't worry about destroying the actual rb->db object,
             since the pool we're using has the lifetime of one
             revision anyway */
          LDR_DBG(("Closing dir %p\n", rb->db->baton));
          SVN_ERR(commit_editor->close_directory(rb->db->baton, rb->pool));
          rb->db = rb->db->parent;
        }

      for (i = 0; i < residual_open_path->nelts; i ++)
        {
          relpath_compose =
            svn_relpath_join(rb->db->relpath,
                             APR_ARRAY_IDX(residual_open_path, i, const char *),
                             rb->pool);
          SVN_ERR(commit_editor->open_directory(relpath_compose,
                                                rb->db->baton,
                                                rb->rev - 1,
                                                rb->pool, &child_baton));
          LDR_DBG(("Opened dir %p\n", child_baton));
          child_db = apr_pcalloc(rb->pool, sizeof(*child_db));
          child_db->baton = child_baton;
          child_db->depth = rb->db->depth + 1;
          child_db->relpath = relpath_compose;
          child_db->parent = rb->db;
          rb->db = child_db;
        }
    }

  switch (nb->action)
    {
    case svn_node_action_delete:
    case svn_node_action_replace:
      LDR_DBG(("Deleting entry %s in %p\n", nb->path, rb->db->baton));
      SVN_ERR(commit_editor->delete_entry(nb->path, rb->rev,
                                          rb->db->baton, rb->pool));
      if (nb->action == svn_node_action_delete)
        break;
      else
        /* FALL THROUGH */;
    case svn_node_action_add:
      switch (nb->kind)
        {
        case svn_node_file:
          SVN_ERR(commit_editor->add_file(nb->path, rb->db->baton,
                                          nb->copyfrom_path,
                                          nb->copyfrom_rev,
                                          rb->pool, &(nb->file_baton)));
          LDR_DBG(("Added file %s to dir %p as %p\n",
                   nb->path, rb->db->baton, nb->file_baton));
          break;
        case svn_node_dir:
          SVN_ERR(commit_editor->add_directory(nb->path, rb->db->baton,
                                               nb->copyfrom_path,
                                               nb->copyfrom_rev,
                                               rb->pool, &child_baton));
          LDR_DBG(("Added dir %s to dir %p as %p\n",
                   nb->path, rb->db->baton, child_baton));
          child_db = apr_pcalloc(rb->pool, sizeof(*child_db));
          child_db->baton = child_baton;
          child_db->depth = rb->db->depth + 1;
          child_db->relpath = apr_pstrdup(rb->pool, nb->path);
          child_db->parent = rb->db;
          rb->db = child_db;
          break;
        default:
          break;
        }
      break;
    case svn_node_action_change:
      switch (nb->kind)
        {
        case svn_node_file:
          /* open_file to set the file_baton so we can apply props,
             txdelta to it */
          SVN_ERR(commit_editor->open_file(nb->path, rb->db->baton,
                                           SVN_INVALID_REVNUM, rb->pool,
                                           &(nb->file_baton)));
          break;
        default:
          /* The directory baton has already been set */
          break;
        }
      break;
    }

  *node_baton = nb;
  return SVN_NO_ERROR;
}

static svn_error_t *
set_revision_property(void *baton,
                      const char *name,
                      const svn_string_t *value)
{
  struct revision_baton *rb = baton;

  SVN_ERR(svn_repos__validate_prop(name, value, rb->pool));

  if (rb->rev > 0)
    apr_hash_set(rb->revprop_table, apr_pstrdup(rb->pool, name),
                 APR_HASH_KEY_STRING, svn_string_dup(value, rb->pool));
  else
    /* Special handling for revision 0; this is safe because the
       commit_editor hasn't been created yet. */
    SVN_ERR(svn_ra_change_rev_prop2(rb->pb->session, rb->rev,
                                    name, NULL, value, rb->pool));

  /* Remember any datestamp/ author that passes through (see comment
     in close_revision). */
  if (!strcmp(name, SVN_PROP_REVISION_DATE))
    rb->datestamp = svn_string_dup(value, rb->pool);
  if (!strcmp(name, SVN_PROP_REVISION_AUTHOR))
    rb->author = svn_string_dup(value, rb->pool);

  return SVN_NO_ERROR;
}

static svn_error_t *
set_node_property(void *baton,
                  const char *name,
                  const svn_string_t *value)
{
  struct node_baton *nb = baton;
  const struct svn_delta_editor_t *commit_editor = nb->rb->pb->commit_editor;
  apr_pool_t *pool = nb->rb->pool;

  SVN_ERR(svn_repos__validate_prop(name, value, pool));

  switch (nb->kind)
    {
    case svn_node_file:
      LDR_DBG(("Applying properties on %p\n", nb->file_baton));
      SVN_ERR(commit_editor->change_file_prop(nb->file_baton, name,
                                              value, pool));
      break;
    case svn_node_dir:
      LDR_DBG(("Applying properties on %p\n", nb->rb->db->baton));
      SVN_ERR(commit_editor->change_dir_prop(nb->rb->db->baton, name,
                                             value, pool));
      break;
    default:
      break;
    }
  return SVN_NO_ERROR;
}

static svn_error_t *
delete_node_property(void *baton,
                     const char *name)
{
  struct node_baton *nb = baton;
  const struct svn_delta_editor_t *commit_editor = nb->rb->pb->commit_editor;
  apr_pool_t *pool = nb->rb->pool;

  SVN_ERR(svn_repos__validate_prop(name, NULL, pool));

  if (nb->kind == svn_node_file)
    SVN_ERR(commit_editor->change_file_prop(nb->file_baton, name,
                                            NULL, pool));
  else
    SVN_ERR(commit_editor->change_dir_prop(nb->rb->db->baton, name,
                                           NULL, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
remove_node_props(void *baton)
{
  struct node_baton *nb = baton;
  apr_pool_t *pool = nb->rb->pool;
  apr_hash_index_t *hi;
  apr_hash_t *props;

  if ((nb->action == svn_node_action_add
            || nb->action == svn_node_action_replace)
      && ! SVN_IS_VALID_REVNUM(nb->copyfrom_rev))
    /* Add-without-history; no "old" properties to worry about. */
    return SVN_NO_ERROR;

  if (nb->kind == svn_node_file)
    {
      SVN_ERR(svn_ra_get_file(nb->rb->pb->session, nb->path, SVN_INVALID_REVNUM,
                              NULL, NULL, &props, pool));
    }
  else  /* nb->kind == svn_node_dir */
    {
      SVN_ERR(svn_ra_get_dir2(nb->rb->pb->session, NULL, NULL, &props, nb->path,
                              SVN_INVALID_REVNUM, 0, pool));
    }

  for (hi = apr_hash_first(pool, props); hi; hi = apr_hash_next(hi))
    {
      const char *name = svn__apr_hash_index_key(hi);
      svn_prop_kind_t kind = svn_property_kind(NULL, name);

      if (kind == svn_prop_regular_kind)
        SVN_ERR(set_node_property(nb, name, NULL));
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
set_fulltext(svn_stream_t **stream,
             void *node_baton)
{
  struct node_baton *nb = node_baton;
  const struct svn_delta_editor_t *commit_editor = nb->rb->pb->commit_editor;
  svn_txdelta_window_handler_t handler;
  void *handler_baton;
  apr_pool_t *pool = nb->rb->pool;

  LDR_DBG(("Setting fulltext for %p\n", nb->file_baton));
  SVN_ERR(commit_editor->apply_textdelta(nb->file_baton, nb->base_checksum,
                                         pool, &handler, &handler_baton));
  *stream = svn_txdelta_target_push(handler, handler_baton,
                                    svn_stream_empty(pool), pool);
  return SVN_NO_ERROR;
}

static svn_error_t *
apply_textdelta(svn_txdelta_window_handler_t *handler,
                void **handler_baton,
                void *node_baton)
{
  struct node_baton *nb = node_baton;
  const struct svn_delta_editor_t *commit_editor = nb->rb->pb->commit_editor;
  apr_pool_t *pool = nb->rb->pool;

  LDR_DBG(("Applying textdelta to %p\n", nb->file_baton));
  SVN_ERR(commit_editor->apply_textdelta(nb->file_baton, nb->base_checksum,
                                         pool, handler, handler_baton));

  return SVN_NO_ERROR;
}

static svn_error_t *
close_node(void *baton)
{
  struct node_baton *nb = baton;
  const struct svn_delta_editor_t *commit_editor = nb->rb->pb->commit_editor;

  /* Pass a file node closure through to the editor *unless* we
     deleted the file (which doesn't require us to open it). */
  if ((nb->kind == svn_node_file) && (nb->file_baton))
    {
      LDR_DBG(("Closing file %p\n", nb->file_baton));
      SVN_ERR(commit_editor->close_file(nb->file_baton, NULL, nb->rb->pool));
    }

  /* The svn_node_dir case is handled in close_revision */

  return SVN_NO_ERROR;
}

static svn_error_t *
close_revision(void *baton)
{
  struct revision_baton *rb = baton;
  const svn_delta_editor_t *commit_editor = rb->pb->commit_editor;
  void *commit_edit_baton = rb->pb->commit_edit_baton;

  /* Fake revision 0 */
  if (rb->rev == 0)
    /* ### Don't print directly; generate a notification. */
    SVN_ERR(svn_cmdline_printf(rb->pool, "* Loaded revision 0.\n"));
  else if (commit_editor)
    {
      /* Close all pending open directories, and then close the edit
         session itself */
      while (rb->db && rb->db->parent)
        {
          LDR_DBG(("Closing dir %p\n", rb->db->baton));
          SVN_ERR(commit_editor->close_directory(rb->db->baton, rb->pool));
          rb->db = rb->db->parent;
        }
      /* root dir's baton */
      LDR_DBG(("Closing edit on %p\n", commit_edit_baton));
      SVN_ERR(commit_editor->close_directory(rb->db->baton, rb->pool));
      SVN_ERR(commit_editor->close_edit(commit_edit_baton, rb->pool));
    }
  else
    {
      void *child_baton;

      /* Legitimate revision with no node information */
      SVN_ERR(svn_ra_get_commit_editor3(rb->pb->session, &commit_editor,
                                        &commit_edit_baton, rb->revprop_table,
                                        commit_callback, NULL, NULL, FALSE,
                                        rb->pool));

      SVN_ERR(commit_editor->open_root(commit_edit_baton, rb->rev - 1,
                                       rb->pool, &child_baton));

      LDR_DBG(("Opened root %p\n", child_baton));
      LDR_DBG(("Closing edit on %p\n", commit_edit_baton));
      SVN_ERR(commit_editor->close_directory(child_baton, rb->pool));
      SVN_ERR(commit_editor->close_edit(commit_edit_baton, rb->pool));
    }

  /* svn_fs_commit_txn() rewrites the datestamp and author properties;
     we'll rewrite them again by hand after closing the commit_editor. */
  SVN_ERR(svn_repos__validate_prop(SVN_PROP_REVISION_DATE,
                                   rb->datestamp, rb->pool));
  SVN_ERR(svn_ra_change_rev_prop2(rb->pb->session, rb->rev,
                                  SVN_PROP_REVISION_DATE,
                                  NULL, rb->datestamp, rb->pool));
  SVN_ERR(svn_repos__validate_prop(SVN_PROP_REVISION_AUTHOR,
                                   rb->author, rb->pool));
  SVN_ERR(svn_ra_change_rev_prop2(rb->pb->session, rb->rev,
                                  SVN_PROP_REVISION_AUTHOR,
                                  NULL, rb->author, rb->pool));

  svn_pool_destroy(rb->pool);

  return SVN_NO_ERROR;
}

svn_error_t *
load_dumpstream(svn_stream_t *stream,
                svn_ra_session_t *session,
                svn_cancel_func_t cancel_func,
                void *cancel_baton,
                apr_pool_t *pool)
{
  svn_repos_parse_fns2_t *parser;
  struct parse_baton *parse_baton;
  const svn_string_t *lock_string;
  svn_boolean_t be_atomic;
  svn_error_t *err;
  const char *root_url;

  SVN_ERR(svn_ra_has_capability(session, &be_atomic,
                                SVN_RA_CAPABILITY_ATOMIC_REVPROPS,
                                pool));
  SVN_ERR(get_lock(&lock_string, session, cancel_func, cancel_baton, pool));
  SVN_ERR(svn_ra_get_repos_root2(session, &root_url, pool));

  parser = apr_pcalloc(pool, sizeof(*parser));
  parser->new_revision_record = new_revision_record;
  parser->uuid_record = uuid_record;
  parser->new_node_record = new_node_record;
  parser->set_revision_property = set_revision_property;
  parser->set_node_property = set_node_property;
  parser->delete_node_property = delete_node_property;
  parser->remove_node_props = remove_node_props;
  parser->set_fulltext = set_fulltext;
  parser->apply_textdelta = apply_textdelta;
  parser->close_node = close_node;
  parser->close_revision = close_revision;

  parse_baton = apr_pcalloc(pool, sizeof(*parse_baton));
  parse_baton->session = session;
  parse_baton->root_url = root_url;

  err = svn_repos_parse_dumpstream2(stream, parser, parse_baton,
                                    cancel_func, cancel_baton, pool);

  /* If all goes well, or if we're cancelled cleanly, don't leave a
     stray lock behind. */
  if ((! err) || (err && (err->apr_err == SVN_ERR_CANCELLED)))
    err = svn_error_compose_create(
              svn_ra__release_operational_lock(session, SVNRDUMP_PROP_LOCK,
                                               lock_string, pool),
              err);
  return err;
}
