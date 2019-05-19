/*
  +----------------------------------------------------------------------+
  | parallel                                                              |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2019                                       |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: krakjoe                                                      |
  +----------------------------------------------------------------------+
 */
#ifndef HAVE_PARALLEL_PARALLEL
#define HAVE_PARALLEL_PARALLEL

#include "parallel.h"

TSRM_TLS struct {
    zend_string *bootstrap;
    HashTable    runtimes;
} php_parallel_globals;

#define PCG(e) php_parallel_globals.e

/* {{{ */
typedef int (*php_sapi_deactivate_t)(void);
typedef size_t (*php_sapi_output_t)(const char*, size_t);

static php_sapi_deactivate_t php_sapi_deactivate_function;
static php_sapi_output_t     php_sapi_output_function;

static pthread_mutex_t php_parallel_output_mutex = PTHREAD_MUTEX_INITIALIZER;

static size_t php_parallel_output_function(const char *str, size_t len) {
    size_t result;

    pthread_mutex_lock(&php_parallel_output_mutex);
    result =
        php_sapi_output_function(str, len);
    pthread_mutex_unlock(&php_parallel_output_mutex);

    return result;
} /* }}} */

/* {{{ */
ZEND_BEGIN_ARG_INFO_EX(php_parallel_bootstrap_arginfo, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, file, IS_STRING, 0)
ZEND_END_ARG_INFO()

static PHP_NAMED_FUNCTION(php_parallel_bootstrap)
{
    zend_string *bootstrap;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_PATH_STR(bootstrap)
    ZEND_PARSE_PARAMETERS_END();

    if (PCG(bootstrap)) {
        zend_string_release(PCG(bootstrap));
    }

    PCG(bootstrap) = zend_string_copy(bootstrap);
} /* }}} */

/* {{{ */
static zend_always_inline php_parallel_runtime_t* php_parallel_runtimes_fetch() {
    php_parallel_runtime_t *runtime;

    ZEND_HASH_FOREACH_PTR(&PCG(runtimes), runtime) {
        if (!php_parallel_scheduler_busy(runtime)) {
            return runtime;
        }
    } ZEND_HASH_FOREACH_END();

    if (!(runtime = php_parallel_runtime_construct(PCG(bootstrap)))) {
        return NULL;
    }

    return zend_hash_next_index_insert_ptr(&PCG(runtimes), runtime);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(php_parallel_run_arginfo, 0, 1, \\parallel\\Future, 1)
    ZEND_ARG_OBJ_INFO(0, task, Closure, 0)
    ZEND_ARG_TYPE_INFO(0, argv, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

static PHP_NAMED_FUNCTION(php_parallel_run)
{
    php_parallel_runtime_t *runtime;
    zval *closure = NULL;
    zval *argv = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJECT_OF_CLASS(closure, zend_ce_closure)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(argv)
    ZEND_PARSE_PARAMETERS_END();

    runtime = php_parallel_runtimes_fetch();

    if (!EG(exception)) {
        php_parallel_scheduler_push(
            runtime, closure, argv, return_value);
    }
} /* }}} */

/* {{{ */
zend_function_entry php_parallel_functions[] = {
	ZEND_NS_FENTRY("parallel", bootstrap, php_parallel_bootstrap, php_parallel_bootstrap_arginfo, 0)
	ZEND_NS_FENTRY("parallel", run,       php_parallel_run,       php_parallel_run_arginfo, 0)

	ZEND_NS_FENTRY("parallel", make, zim_Channel_make, php_parallel_channel_make_arginfo, 0)
	ZEND_NS_FENTRY("parallel", open, zim_Channel_open, php_parallel_channel_open_arginfo, 0)
    PHP_FE_END
}; /* }}} */

PHP_MINIT_FUNCTION(PARALLEL_CORE)
{
    if (strncmp(sapi_module.name, "cli", sizeof("cli")-1) == SUCCESS) {
        php_sapi_deactivate_function = sapi_module.deactivate;

        sapi_module.deactivate = NULL;
    }

    php_sapi_output_function = sapi_module.ub_write;

    sapi_module.ub_write = php_parallel_output_function;

    PHP_MINIT(PARALLEL_HANDLERS)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MINIT(PARALLEL_EXCEPTIONS)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MINIT(PARALLEL_COPY)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MINIT(PARALLEL_SCHEDULER)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MINIT(PARALLEL_CHANNEL)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MINIT(PARALLEL_EVENTS)(INIT_FUNC_ARGS_PASSTHRU);

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(PARALLEL_CORE)
{
    PHP_MSHUTDOWN(PARALLEL_EVENTS)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MSHUTDOWN(PARALLEL_CHANNEL)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MSHUTDOWN(PARALLEL_SCHEDULER)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MSHUTDOWN(PARALLEL_COPY)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MSHUTDOWN(PARALLEL_EXCEPTIONS)(INIT_FUNC_ARGS_PASSTHRU);
    PHP_MSHUTDOWN(PARALLEL_HANDLERS)(INIT_FUNC_ARGS_PASSTHRU);

    if (strncmp(sapi_module.name, "cli", sizeof("cli")-1) == SUCCESS) {
        sapi_module.deactivate = php_sapi_deactivate_function;
    }

    sapi_module.ub_write = php_sapi_output_function;

    return SUCCESS;
}

static void php_parallel_runtimes_release(zval *zv) {
    php_parallel_runtime_t *runtime =
        (php_parallel_runtime_t*) Z_PTR_P(zv);

    OBJ_RELEASE(&runtime->std);
}

PHP_RINIT_FUNCTION(PARALLEL_CORE)
{
    PHP_RINIT(PARALLEL_COPY)(INIT_FUNC_ARGS_PASSTHRU);

    zend_hash_init(
        &PCG(runtimes), 16, NULL, php_parallel_runtimes_release, 0);
    PCG(bootstrap) = NULL;

	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(PARALLEL_CORE)
{
    if (!CG(unclean_shutdown)) {
        PHP_RSHUTDOWN(PARALLEL_COPY)(INIT_FUNC_ARGS_PASSTHRU);
    }

    zend_hash_destroy(&PCG(runtimes));

    if (PCG(bootstrap)) {
        zend_string_release(PCG(bootstrap));
    }

	return SUCCESS;
}
#endif
