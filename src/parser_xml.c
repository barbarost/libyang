/**
 * @file parser_xml.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief XML data parser for libyang
 *
 * Copyright (c) 2015 - 2017 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "libyang.h"
#include "common.h"
#include "context.h"
#include "parser.h"
#include "tree_internal.h"
#include "validation.h"
#include "xml_internal.h"

/* does not log */
static struct lys_node *
xml_data_search_schemanode(struct lyxml_elem *xml, struct lys_node *start, int options)
{
    struct lys_node *result, *aux;

    LY_TREE_FOR(start, result) {
        /* skip groupings ... */
        if (result->nodetype == LYS_GROUPING) {
            continue;
        /* ... and output in case of RPC ... */
        } else if (result->nodetype == LYS_OUTPUT && (options & LYD_OPT_RPC)) {
            continue;
        /* ... and input in case of RPC reply */
        } else if (result->nodetype == LYS_INPUT && (options & LYD_OPT_RPCREPLY)) {
            continue;
        }

        /* go into cases, choices, uses and in RPCs into input and output */
        if (result->nodetype & (LYS_CHOICE | LYS_CASE | LYS_USES | LYS_INPUT | LYS_OUTPUT)) {
            aux = xml_data_search_schemanode(xml, result->child, options);
            if (aux) {
                /* we have matching result */
                return aux;
            }
            /* else, continue with next schema node */
            continue;
        }

        /* match data nodes */
        if (ly_strequal(result->name, xml->name, 1)) {
            /* names matches, what about namespaces? */
            if (ly_strequal(lys_main_module(result->module)->ns, xml->ns->value, 1)) {
                /* we have matching result */
                return result;
            }
            /* else, continue with next schema node */
            continue;
        }
    }

    /* no match */
    return NULL;
}

/* logs directly */
static int
xml_get_value(struct lyd_node *node, struct lyxml_elem *xml, int editbits)
{
    struct lyd_node_leaf_list *leaf = (struct lyd_node_leaf_list *)node;

    assert(node && (node->schema->nodetype & (LYS_LEAFLIST | LYS_LEAF)) && xml);

    leaf->value_str = lydict_insert(node->schema->module->ctx, xml->content, 0);

    if ((editbits & 0x20) && (node->schema->nodetype & LYS_LEAF) && (!leaf->value_str || !leaf->value_str[0])) {
        /* we have edit-config leaf/leaf-list with delete operation and no (empty) value,
         * this is not a bug since the node is just used as a kind of selection node */
        leaf->value_type = LY_TYPE_ERR;
        return EXIT_SUCCESS;
    }

    /* the value is here converted to a JSON format if needed in case of LY_TYPE_IDENT and LY_TYPE_INST or to a
     * canonical form of the value */
    if (!lyp_parse_value(&((struct lys_node_leaf *)leaf->schema)->type, &leaf->value_str, xml, leaf, NULL, 1, 0)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/* logs directly */
static int
xml_parse_data(struct ly_ctx *ctx, struct lyxml_elem *xml, struct lyd_node *parent, struct lyd_node *first_sibling,
               struct lyd_node *prev, int options, struct unres_data *unres, struct lyd_node **result,
               struct lyd_node **act_notif)
{
    const struct lys_module *mod = NULL;
    struct lyd_node *diter, *dlast;
    struct lys_node *schema = NULL, *target;
    struct lys_node_augment *aug;
    struct lyd_attr *dattr, *dattr_iter;
    struct lyxml_attr *attr;
    struct lyxml_elem *child, *next;
    int i, j, havechildren, r, editbits = 0, pos, filterflag = 0, found;
    int ret = 0;
    const char *str = NULL;

    assert(xml);
    assert(result);
    *result = NULL;

    if (xml->flags & LYXML_ELEM_MIXED) {
        if (options & LYD_OPT_STRICT) {
            LOGVAL(LYE_XML_INVAL, LY_VLOG_XML, xml, "XML element with mixed content");
            return -1;
        } else {
            return 0;
        }
    }

    if (!xml->ns || !xml->ns->value) {
        if (options & LYD_OPT_STRICT) {
            LOGVAL(LYE_XML_MISS, LY_VLOG_XML, xml, "element's", "namespace");
            return -1;
        } else {
            return 0;
        }
    }

    /* find schema node */
    if (!parent) {
        mod = ly_ctx_get_module_by_ns(ctx, xml->ns->value, NULL);
        if (ctx->data_clb) {
            if (!mod) {
                mod = ctx->data_clb(ctx, NULL, xml->ns->value, 0, ctx->data_clb_data);
            } else if (!mod->implemented) {
                mod = ctx->data_clb(ctx, mod->name, mod->ns, LY_MODCLB_NOT_IMPLEMENTED, ctx->data_clb_data);
            }
        }

        /* get the proper schema node */
        if (mod && mod->implemented && !mod->disabled) {
            schema = xml_data_search_schemanode(xml, mod->data, options);
            if (!schema) {
                /* it still can be the specific case of this module containing an augment of another module
                * top-level choice or top-level choice's case, bleh */
                for (j = 0; j < mod->augment_size; ++j) {
                    aug = &mod->augment[j];
                    target = aug->target;
                    if (target->nodetype & (LYS_CHOICE | LYS_CASE)) {
                        /* 1) okay, the target is choice or case */
                        while (target && (target->nodetype & (LYS_CHOICE | LYS_CASE | LYS_USES))) {
                            target = lys_parent(target);
                        }
                        /* 2) now, the data node will be top-level, there are only non-data schema nodes */
                        if (!target) {
                            while ((schema = (struct lys_node *)lys_getnext(schema, (struct lys_node *)aug, NULL, 0))) {
                                /* 3) alright, even the name matches, we found our schema node */
                                if (ly_strequal(schema->name, xml->name, 1)) {
                                    break;
                                }
                            }
                        }
                    }

                    if (schema) {
                        break;
                    }
                }
            }
        }
    } else {
        /* parsing some internal node, we start with parent's schema pointer */
        schema = xml_data_search_schemanode(xml, parent->schema->child, options);

        if (ctx->data_clb) {
            if (schema && !lys_node_module(schema)->implemented) {
                ctx->data_clb(ctx, lys_node_module(schema)->name, lys_node_module(schema)->ns,
                              LY_MODCLB_NOT_IMPLEMENTED, ctx->data_clb_data);
            } else if (!schema) {
                if (ctx->data_clb(ctx, NULL, xml->ns->value, 0, ctx->data_clb_data)) {
                    /* context was updated, so try to find the schema node again */
                    schema = xml_data_search_schemanode(xml, parent->schema->child, options);
                }
            }
        }
    }

    mod = lys_node_module(schema);
    if (!mod || !mod->implemented || mod->disabled) {
        if (options & LYD_OPT_STRICT) {
            LOGVAL(LYE_INELEM, (parent ? LY_VLOG_LYD : LY_VLOG_NONE), parent, xml->name);
            return -1;
        } else {
            return 0;
        }
    }

    /* create the element structure */
    switch (schema->nodetype) {
    case LYS_CONTAINER:
    case LYS_LIST:
    case LYS_NOTIF:
    case LYS_RPC:
    case LYS_ACTION:
        *result = calloc(1, sizeof **result);
        havechildren = 1;
        break;
    case LYS_LEAF:
    case LYS_LEAFLIST:
        *result = calloc(1, sizeof(struct lyd_node_leaf_list));
        havechildren = 0;
        break;
    case LYS_ANYXML:
    case LYS_ANYDATA:
        *result = calloc(1, sizeof(struct lyd_node_anydata));
        havechildren = 0;
        break;
    default:
        LOGINT;
        return -1;
    }
    LY_CHECK_ERR_RETURN(!(*result), LOGMEM, -1);

    (*result)->prev = *result;
    (*result)->schema = schema;
    (*result)->parent = parent;
    diter = NULL;
    if (parent && parent->child && schema->nodetype == LYS_LEAF && parent->schema->nodetype == LYS_LIST &&
        (pos = lys_is_key((struct lys_node_list *)parent->schema, (struct lys_node_leaf *)schema))) {
        /* it is key and we need to insert it into a correct place */
        for (i = 0, diter = parent->child;
                diter && i < (pos - 1) && diter->schema->nodetype == LYS_LEAF &&
                    lys_is_key((struct lys_node_list *)parent->schema, (struct lys_node_leaf *)diter->schema);
                i++, diter = diter->next);
        if (diter) {
            /* out of order insertion - insert list's key to the correct position, before the diter */
            if (options & LYD_OPT_STRICT) {
                LOGVAL(LYE_INORDER, LY_VLOG_LYD, *result, schema->name, diter->schema->name);
                LOGVAL(LYE_SPEC, LY_VLOG_PREV, NULL, "Invalid position of the key \"%s\" in a list \"%s\".",
                       schema->name, parent->schema->name);
                free(*result);
                *result = NULL;
                return -1;
            } else {
                LOGWRN("Invalid position of the key \"%s\" in a list \"%s\".", schema->name, parent->schema->name)
            }
            if (parent->child == diter) {
                parent->child = *result;
                /* update first_sibling */
                first_sibling = *result;
            }
            if (diter->prev->next) {
                diter->prev->next = *result;
            }
            (*result)->prev = diter->prev;
            diter->prev = *result;
            (*result)->next = diter;
        }
    }
    if (!diter) {
        /* simplified (faster) insert as the last node */
        if (parent && !parent->child) {
            parent->child = *result;
        }
        if (prev) {
            (*result)->prev = prev;
            prev->next = *result;

            /* fix the "last" pointer */
            first_sibling->prev = *result;
        } else {
            (*result)->prev = *result;
            first_sibling = *result;
        }
    }
    (*result)->validity = ly_new_node_validity((*result)->schema);
    if (resolve_applies_when(schema, 0, NULL)) {
        (*result)->when_status = LYD_WHEN;
    }

    /* process attributes */
    for (attr = xml->attr; attr; attr = attr->next) {
        if (attr->type != LYXML_ATTR_STD) {
            continue;
        } else if (!attr->ns) {
            if ((*result)->schema->nodetype == LYS_ANYXML &&
                    ly_strequal((*result)->schema->name, "filter", 0) &&
                    (ly_strequal((*result)->schema->module->name, "ietf-netconf", 0) ||
                    ly_strequal((*result)->schema->module->name, "notifications", 0))) {
                /* NETCONF filter's attributes, which we implement as non-standard annotations,
                 * they are unqualified (no namespace), but we know that we have internally defined
                 * them in the ietf-netconf module */
                str = "urn:ietf:params:xml:ns:netconf:base:1.0";
                filterflag = 1;
            } else {
                /* garbage */
                goto attr_error;
            }
        } else {
            str = attr->ns->value;
        }

        r = lyp_fill_attr(ctx, *result, str, NULL, attr->name, attr->value, xml, &dattr);
        if (r == -1) {
            goto error;
        } else if (r == 1) {
attr_error:
            if (options & LYD_OPT_STRICT) {
                LOGVAL(LYE_INMETA, LY_VLOG_LYD, *result, (attr->ns ? attr->ns->prefix : "<none>"), attr->name, attr->value);
                goto error;
            }

            LOGWRN("Unknown \"%s:%s\" metadata with value \"%s\", ignoring.",
                   (attr->ns ? attr->ns->prefix : "<none>"), attr->name, attr->value);
            continue;
        }

        /* special case of xpath in the value, we want to convert it to JSON */
        if (filterflag && !strcmp(attr->name, "select")) {
            dattr->value.string = transform_xml2json(ctx, dattr->value_str, xml, 0, 1);
            if (!dattr->value.string) {
                /* problem with resolving value as xpath */
                dattr->value.string = dattr->value_str;
                goto error;
            }
            lydict_remove(ctx, dattr->value_str);
            dattr->value_str = dattr->value.string;
        }

        /* insert into the data node */
        if (!(*result)->attr) {
            (*result)->attr = dattr;
        } else {
            for (dattr_iter = (*result)->attr; dattr_iter->next; dattr_iter = dattr_iter->next);
            dattr_iter->next = dattr;
        }
        continue;
    }

    /* check insert attribute and its values */
    if (options & LYD_OPT_EDIT) {
        if (lyp_check_edit_attr(ctx, (*result)->attr, *result, &editbits)) {
            goto error;
        }

    /* check correct filter extension attributes */
    } else if (filterflag) {
        found = 0; /* 0 - nothing, 1 - type subtree, 2 - type xpath, 3 - select, 4 - type xpath + select */
        LY_TREE_FOR((*result)->attr, dattr_iter) {
            if (!strcmp(dattr_iter->name, "type")) {
                if ((found == 1) || (found == 2) || (found == 4)) {
                    LOGVAL(LYE_TOOMANY, LY_VLOG_LYD, (*result), "type", xml->name);
                    goto error;
                }
                switch (dattr_iter->value.enm->value) {
                case 0:
                    /* subtree */
                    if (found == 3) {
                        LOGVAL(LYE_INATTR, LY_VLOG_LYD, (*result), dattr_iter->name);
                        goto error;
                    }

                    assert(!found);
                    found = 1;
                    break;
                case 1:
                    /* xpath */
                    if (found == 3) {
                        found = 4;
                    } else {
                        assert(!found);
                        found = 2;
                    }
                    break;
                default:
                    LOGINT;
                    goto error;
                }
            } else if (!strcmp(dattr_iter->name, "select")) {
                switch (found) {
                case 0:
                    found = 3;
                    break;
                case 1:
                    LOGVAL(LYE_INATTR, LY_VLOG_LYD, (*result), dattr_iter->name);
                    goto error;
                case 2:
                    found = 4;
                    break;
                case 3:
                case 4:
                    LOGVAL(LYE_TOOMANY, LY_VLOG_LYD, (*result), "select", xml->name);
                    goto error;
                default:
                    LOGINT;
                    goto error;
                }
            }
        }

        /* check if what we found is correct */
        switch (found) {
        case 1:
        case 4:
            /* ok */
            break;
        case 2:
            LOGVAL(LYE_MISSATTR, LY_VLOG_LYD, (*result), "select", xml->name);
            goto error;
        case 3:
            LOGVAL(LYE_MISSATTR, LY_VLOG_LYD, (*result), "type", xml->name);
            goto error;
        default:
            LOGINT;
            goto error;
        }
    }

    /* type specific processing */
    if (schema->nodetype & (LYS_LEAF | LYS_LEAFLIST)) {
        /* type detection and assigning the value */
        if (xml_get_value(*result, xml, editbits)) {
            goto error;
        }
    } else if (schema->nodetype & LYS_ANYDATA) {
        /* store children values */
        if (xml->child) {
            child = xml->child;
            /* manually unlink all siblings and correct namespaces */
            xml->child = NULL;
            LY_TREE_FOR(child, next) {
                next->parent = NULL;
                lyxml_correct_elem_ns(ctx, next, 1, 1);
            }

            ((struct lyd_node_anydata *)*result)->value_type = LYD_ANYDATA_XML;
            ((struct lyd_node_anydata *)*result)->value.xml = child;
        } else {
            ((struct lyd_node_anydata *)*result)->value_type = LYD_ANYDATA_CONSTSTRING;
            ((struct lyd_node_anydata *)*result)->value.str = lydict_insert(ctx, xml->content, 0);
        }
    } else if (schema->nodetype & (LYS_RPC | LYS_ACTION)) {
        if (!(options & LYD_OPT_RPC) || *act_notif) {
            LOGVAL(LYE_INELEM, LY_VLOG_LYD, (*result), schema->name);
            LOGVAL(LYE_SPEC, LY_VLOG_PREV, NULL, "Unexpected %s node \"%s\".",
                   (schema->nodetype == LYS_RPC ? "rpc" : "action"), schema->name);
            goto error;
        }
        *act_notif = *result;
    } else if (schema->nodetype == LYS_NOTIF) {
        if (!(options & LYD_OPT_NOTIF) || *act_notif) {
            LOGVAL(LYE_INELEM, LY_VLOG_LYD, (*result), schema->name);
            LOGVAL(LYE_SPEC, LY_VLOG_PREV, NULL, "Unexpected notification node \"%s\".", schema->name);
            goto error;
        }
        *act_notif = *result;
    }

    /* first part of validation checks */
    if (lyv_data_context(*result, options, unres)) {
        goto error;
    }

    /* process children */
    if (havechildren && xml->child) {
        diter = dlast = NULL;
        LY_TREE_FOR_SAFE(xml->child, next, child) {
            r = xml_parse_data(ctx, child, *result, (*result)->child, dlast, options, unres, &diter, act_notif);
            if (r) {
                goto error;
            } else if (options & LYD_OPT_DESTRUCT) {
                lyxml_free(ctx, child);
            }
            if (diter && !diter->next) {
                /* the child was parsed/created and it was placed as the last child. The child can be inserted
                 * out of order (not as the last one) in case it is a list's key present out of the correct order */
                dlast = diter;
            }
        }
    }

    /* if we have empty non-presence container, we keep it, but mark it as default */
    if (schema->nodetype == LYS_CONTAINER && !(*result)->child &&
            !(*result)->attr && !((struct lys_node_container *)schema)->presence) {
        (*result)->dflt = 1;
    }

    /* rest of validation checks */
    ly_err_clean(1);
    if (lyv_data_content(*result, options, unres) ||
             lyv_multicases(*result, NULL, prev ? &first_sibling : NULL, 0, NULL)) {
        if (ly_errno) {
            goto error;
        } else {
            goto clear;
        }
    }

    /* validation successful */
    if ((*result)->schema->nodetype & (LYS_LIST | LYS_LEAFLIST)) {
        /* postpone checking when there will be all list/leaflist instances */
        (*result)->validity |= LYD_VAL_UNIQUE;
    }

    return ret;

error:
    ret--;

clear:
    /* cleanup */
    for (i = unres->count - 1; i >= 0; i--) {
        /* remove unres items connected with the node being removed */
        if (unres->node[i] == *result) {
            unres_data_del(unres, i);
        }
    }
    lyd_free(*result);
    *result = NULL;

    return ret;
}

API struct lyd_node *
lyd_parse_xml(struct ly_ctx *ctx, struct lyxml_elem **root, int options, ...)
{
    va_list ap;
    int r, i;
    struct unres_data *unres = NULL;
    const struct lyd_node *rpc_act = NULL, *data_tree = NULL;
    struct lyd_node *result = NULL, *iter, *last, *reply_parent = NULL, *reply_top = NULL, *act_notif = NULL;
    struct lyxml_elem *xmlstart, *xmlelem, *xmlaux, *xmlfree = NULL;
    struct ly_set *set;

    ly_err_clean(1);

    if (!ctx || !root) {
        LOGERR(LY_EINVAL, "%s: Invalid parameter.", __func__);
        return NULL;
    }

    if (lyp_check_options(options, __func__)) {
        return NULL;
    }

    if (!(*root)) {
        /* empty tree - no work is needed */
        lyd_validate(&result, options, ctx);
        return result;
    }

    unres = calloc(1, sizeof *unres);
    LY_CHECK_ERR_RETURN(!unres, LOGMEM, NULL);

    va_start(ap, options);
    if (options & LYD_OPT_RPCREPLY) {
        rpc_act = va_arg(ap, const struct lyd_node *);
        if (!rpc_act || rpc_act->parent || !(rpc_act->schema->nodetype & (LYS_RPC | LYS_LIST | LYS_CONTAINER))) {
            LOGERR(LY_EINVAL, "%s: invalid variable parameter (const struct lyd_node *rpc_act).", __func__);
            goto error;
        }
        if (rpc_act->schema->nodetype == LYS_RPC) {
            /* RPC request */
            reply_top = reply_parent = _lyd_new(NULL, rpc_act->schema, 0);
        } else {
            /* action request */
            reply_top = lyd_dup(rpc_act, 1);
            LY_TREE_DFS_BEGIN(reply_top, iter, reply_parent) {
                if (reply_parent->schema->nodetype == LYS_ACTION) {
                    break;
                }
                LY_TREE_DFS_END(reply_top, iter, reply_parent);
            }
            if (!reply_parent) {
                LOGERR(LY_EINVAL, "%s: invalid variable parameter (const struct lyd_node *rpc_act).", __func__);
                lyd_free_withsiblings(reply_top);
                goto error;
            }
            lyd_free_withsiblings(reply_parent->child);
        }
    }
    if (options & (LYD_OPT_RPC | LYD_OPT_NOTIF | LYD_OPT_RPCREPLY)) {
        data_tree = va_arg(ap, const struct lyd_node *);
        if (data_tree) {
            LY_TREE_FOR((struct lyd_node *)data_tree, iter) {
                if (iter->parent) {
                    /* a sibling is not top-level */
                    LOGERR(LY_EINVAL, "%s: invalid variable parameter (const struct lyd_node *data_tree).", __func__);
                    goto error;
                }
            }

            /* move it to the beginning */
            for (; data_tree->prev->next; data_tree = data_tree->prev);

            /* LYD_OPT_NOSIBLINGS cannot be set in this case */
            if (options & LYD_OPT_NOSIBLINGS) {
                LOGERR(LY_EINVAL, "%s: invalid parameter (variable arg const struct lyd_node *data_tree with LYD_OPT_NOSIBLINGS).", __func__);
                goto error;
            }
        }
    }

    if (!(options & LYD_OPT_NOSIBLINGS)) {
        /* locate the first root to process */
        if ((*root)->parent) {
            xmlstart = (*root)->parent->child;
        } else {
            xmlstart = *root;
            while(xmlstart->prev->next) {
                xmlstart = xmlstart->prev;
            }
        }
    } else {
        xmlstart = *root;
    }

    if ((options & LYD_OPT_RPC)
            && !strcmp(xmlstart->name, "action") && !strcmp(xmlstart->ns->value, "urn:ietf:params:xml:ns:yang:1")) {
        /* it's an action, not a simple RPC */
        xmlstart = xmlstart->child;
        if (options & LYD_OPT_DESTRUCT) {
            /* free it later */
            xmlfree = xmlstart->parent;
        }
    }

    iter = last = NULL;
    LY_TREE_FOR_SAFE(xmlstart, xmlaux, xmlelem) {
        r = xml_parse_data(ctx, xmlelem, reply_parent, result, last, options, unres, &iter, &act_notif);
        if (r) {
            if (reply_top) {
                result = reply_top;
            }
            goto error;
        } else if (options & LYD_OPT_DESTRUCT) {
            lyxml_free(ctx, xmlelem);
            *root = xmlaux;
        }
        if (iter) {
            last = iter;
        }
        if (!result) {
            result = iter;
        }

        if (options & LYD_OPT_NOSIBLINGS) {
            /* stop after the first processed root */
            break;
        }
    }

    if (reply_top) {
        result = reply_top;
    }

    if ((options & LYD_OPT_RPCREPLY) && (rpc_act->schema->nodetype != LYS_RPC)) {
        /* action reply */
        act_notif = reply_parent;
    } else if ((options & (LYD_OPT_RPC | LYD_OPT_NOTIF)) && !act_notif) {
        ly_vecode = LYVE_INELEM;
        LOGVAL(LYE_SPEC, LY_VLOG_LYD, result, "Missing %s node.", (options & LYD_OPT_RPC ? "action" : "notification"));
        goto error;
    }

    /* check for uniquness of top-level lists/leaflists because
     * only the inner instances were tested in lyv_data_content() */
    set = ly_set_new();
    LY_TREE_FOR(result, iter) {
        if (!(iter->schema->nodetype & (LYS_LIST | LYS_LEAFLIST)) || !(iter->validity & LYD_VAL_UNIQUE)) {
            continue;
        }

        /* check each list/leaflist only once */
        i = set->number;
        if (ly_set_add(set, iter->schema, 0) != i) {
            /* already checked */
            continue;
        }

        if (lyv_data_unique(iter, result)) {
            ly_set_free(set);
            goto error;
        }
    }
    ly_set_free(set);

    /* add default values, resolve unres and check for mandatory nodes in final tree */
    if (lyd_defaults_add_unres(&result, options, ctx, data_tree, act_notif, unres)) {
        goto error;
    }
    if (!(options & (LYD_OPT_TRUSTED | LYD_OPT_NOTIF_FILTER))
            && lyd_check_mandatory_tree((act_notif ? act_notif : result), ctx, options)) {
        goto error;
    }

    if (xmlfree) {
        lyxml_free(ctx, xmlfree);
    }
    free(unres->node);
    free(unres->type);
    free(unres);
    va_end(ap);

    return result;

error:
    lyd_free_withsiblings(result);
    if (xmlfree) {
        lyxml_free(ctx, xmlfree);
    }
    free(unres->node);
    free(unres->type);
    free(unres);
    va_end(ap);

    return NULL;
}
