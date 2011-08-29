/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                           */
/*                  This file is part of the program and library             */
/*         SCIP --- Solving Constraint Integer Programs                      */
/*                                                                           */
/*    Copyright (C) 2002-2011 Konrad-Zuse-Zentrum                            */
/*                            fuer Informationstechnik Berlin                */
/*                                                                           */
/*  SCIP is distributed under the terms of the ZIB Academic License.         */
/*                                                                           */
/*  You should have received a copy of the ZIB Academic License              */
/*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      */
/*                                                                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/**@file   solve.c
 * @brief  main solving loop and node processing
 * @author Tobias Achterberg
 * @author Timo Berthold
 */

/*---+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9----+----0----+----1----+----2*/

#include <assert.h>

#include "scip/def.h"
#include "scip/set.h"
#include "scip/stat.h"
#include "scip/buffer.h"
#include "scip/clock.h"
#include "scip/vbc.h"
#include "scip/interrupt.h"
#include "scip/misc.h"
#include "scip/event.h"
#include "scip/lp.h"
#include "scip/var.h"
#include "scip/prob.h"
#include "scip/sol.h"
#include "scip/primal.h"
#include "scip/tree.h"
#include "scip/pricestore.h"
#include "scip/sepastore.h"
#include "scip/cutpool.h"
#include "scip/solve.h"
#include "scip/scip.h"
#include "scip/branch.h"
#include "scip/conflict.h"
#include "scip/cons.h"
#include "scip/disp.h"
#include "scip/heur.h"
#include "scip/nodesel.h"
#include "scip/pricer.h"
#include "scip/relax.h"
#include "scip/sepa.h"
#include "scip/prop.h"


#define MAXNLPERRORS  10                /**< maximal number of LP error loops in a single node */


/** returns whether the solving process will be / was stopped before proving optimality;
 *  if the solving process was stopped, stores the reason as status in stat
 */
SCIP_Bool SCIPsolveIsStopped(
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_Bool             checknodelimits     /**< should the node limits be involved in the check? */
   )
{
   assert(set != NULL);
   assert(stat != NULL);

   /* in case lowerbound >= upperbound, we do not want to terminate with SCIP_STATUS_GAPLIMIT but with the ordinary 
    * SCIP_STATUS_OPTIMAL/INFEASIBLE/...
    */
   if( set->stage >= SCIP_STAGE_SOLVING && SCIPsetIsLE(set, SCIPgetUpperbound(set->scip), SCIPgetLowerbound(set->scip)) )
      return FALSE;

   /* if some limit has been changed since the last call, we reset the status */
   if( set->limitchanged )
   {
      stat->status = SCIP_STATUS_UNKNOWN;
      set->limitchanged = FALSE;
   }

   if( SCIPinterrupted() || stat->userinterrupt )
   {
      stat->status = SCIP_STATUS_USERINTERRUPT;
      stat->userinterrupt = FALSE;
   }
   else if( SCIPclockGetTime(stat->solvingtime) >= set->limit_time )
      stat->status = SCIP_STATUS_TIMELIMIT;
   else if( SCIPgetMemUsed(set->scip) >= set->limit_memory*1048576.0 )
      stat->status = SCIP_STATUS_MEMLIMIT;
   else if( set->stage >= SCIP_STAGE_SOLVING && SCIPsetIsLT(set, SCIPgetGap(set->scip), set->limit_gap) )
      stat->status = SCIP_STATUS_GAPLIMIT;
   else if( set->stage >= SCIP_STAGE_SOLVING
      && SCIPsetIsLT(set, SCIPgetUpperbound(set->scip) - SCIPgetLowerbound(set->scip), set->limit_absgap) )
      stat->status = SCIP_STATUS_GAPLIMIT;
   else if( set->limit_solutions >= 0 && set->stage >= SCIP_STAGE_PRESOLVED
      && SCIPgetNSolsFound(set->scip) >= set->limit_solutions )
      stat->status = SCIP_STATUS_SOLLIMIT;
   else if( set->limit_bestsol >= 0 && set->stage >= SCIP_STAGE_PRESOLVED
      && SCIPgetNBestSolsFound(set->scip) >= set->limit_bestsol )
      stat->status = SCIP_STATUS_BESTSOLLIMIT;
   else if( checknodelimits && set->limit_nodes >= 0 && stat->nnodes >= set->limit_nodes )
      stat->status = SCIP_STATUS_NODELIMIT;
   else if( checknodelimits && set->limit_stallnodes >= 0 && stat->nnodes >= stat->bestsolnode + set->limit_stallnodes )
      stat->status = SCIP_STATUS_STALLNODELIMIT;

   /* If stat->status was initialized to SCIP_STATUS_NODELIMIT or SCIP_STATUS_STALLNODELIMIT due to a previous call to SCIPsolveIsStopped(,,TRUE),
    * in the case of checknodelimits == FALSE, we do not want to report here that the solve will be stopped due to a nodelimit.
    */
   if( !checknodelimits )
      return (stat->status != SCIP_STATUS_UNKNOWN && stat->status != SCIP_STATUS_NODELIMIT && stat->status != SCIP_STATUS_STALLNODELIMIT);
   else
      return (stat->status != SCIP_STATUS_UNKNOWN);
}

/** calls primal heuristics */
SCIP_RETCODE SCIPprimalHeuristics(
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree, or NULL if called during presolving */
   SCIP_LP*              lp,                 /**< LP data, or NULL if called during presolving or propagation */
   SCIP_NODE*            nextnode,           /**< next node that will be processed, or NULL if no more nodes left or called during presolving */
   SCIP_HEURTIMING       heurtiming,         /**< current point in the node solving process */
   SCIP_Bool*            foundsol            /**< pointer to store whether a solution has been found */
   )
{  /*lint --e{715}*/

   SCIP_RESULT result;
   SCIP_Longint oldnbestsolsfound;
   int ndelayedheurs;
   int depth;
   int lpstateforkdepth;
   int h;

   assert(set != NULL);
   assert(primal != NULL);
   assert(tree != NULL || heurtiming == SCIP_HEURTIMING_BEFOREPRESOL || heurtiming == SCIP_HEURTIMING_DURINGPRESOLLOOP);
   assert(lp != NULL || heurtiming == SCIP_HEURTIMING_BEFOREPRESOL || heurtiming == SCIP_HEURTIMING_DURINGPRESOLLOOP 
      || heurtiming == SCIP_HEURTIMING_AFTERPROPLOOP);
   assert(heurtiming == SCIP_HEURTIMING_BEFORENODE || heurtiming == SCIP_HEURTIMING_DURINGLPLOOP
      || heurtiming == SCIP_HEURTIMING_AFTERLPLOOP || heurtiming == SCIP_HEURTIMING_AFTERNODE
      || heurtiming == SCIP_HEURTIMING_DURINGPRICINGLOOP || heurtiming == SCIP_HEURTIMING_BEFOREPRESOL
      || heurtiming == SCIP_HEURTIMING_DURINGPRESOLLOOP || heurtiming == SCIP_HEURTIMING_AFTERPROPLOOP
      || heurtiming == (SCIP_HEURTIMING_AFTERLPLOOP | SCIP_HEURTIMING_AFTERNODE));
   assert(heurtiming != SCIP_HEURTIMING_AFTERNODE || (nextnode == NULL) == (SCIPtreeGetNNodes(tree) == 0));
   assert(foundsol != NULL);

   *foundsol = FALSE;

   /* nothing to do, if no heuristics are available, or if the branch-and-bound process is finished */
   if( set->nheurs == 0 || (heurtiming == SCIP_HEURTIMING_AFTERNODE && nextnode == NULL) )
      return SCIP_OKAY;

   /* sort heuristics by priority, but move the delayed heuristics to the front */
   SCIPsetSortHeurs(set);

   /* specialize the AFTERNODE timing flag */
   if( (heurtiming & SCIP_HEURTIMING_AFTERNODE) == SCIP_HEURTIMING_AFTERNODE )
   {
      SCIP_Bool plunging;
      SCIP_Bool pseudonode;

      /* clear the AFTERNODE flags and replace them by the right ones */
      heurtiming &= ~SCIP_HEURTIMING_AFTERNODE;

      /* we are in plunging mode iff the next node is a sibling or a child, and no leaf */
      assert(nextnode == NULL
         || SCIPnodeGetType(nextnode) == SCIP_NODETYPE_SIBLING
         || SCIPnodeGetType(nextnode) == SCIP_NODETYPE_CHILD
         || SCIPnodeGetType(nextnode) == SCIP_NODETYPE_LEAF);
      plunging = (nextnode != NULL && SCIPnodeGetType(nextnode) != SCIP_NODETYPE_LEAF);
      pseudonode = !SCIPtreeHasFocusNodeLP(tree);
      if( plunging && SCIPtreeGetCurrentDepth(tree) > 0 ) /* call plunging heuristics also at root node */
      {
         if( !pseudonode )
            heurtiming |= SCIP_HEURTIMING_AFTERLPNODE;
         else
            heurtiming |= SCIP_HEURTIMING_AFTERPSEUDONODE;
      }
      else
      {
         if( !pseudonode )
            heurtiming |= SCIP_HEURTIMING_AFTERLPPLUNGE | SCIP_HEURTIMING_AFTERLPNODE;
         else
            heurtiming |= SCIP_HEURTIMING_AFTERPSEUDOPLUNGE | SCIP_HEURTIMING_AFTERPSEUDONODE;
      }
   }

   /* initialize the tree related data, if we are not in presolving */
   if( heurtiming == SCIP_HEURTIMING_BEFOREPRESOL || heurtiming == SCIP_HEURTIMING_DURINGPRESOLLOOP )
   {
      depth = -1;
      lpstateforkdepth = -1;

      SCIPdebugMessage("calling primal heuristics %s presolving\n", 
         heurtiming == SCIP_HEURTIMING_BEFOREPRESOL ? "before" : "during");
   }
   else
   {
      assert(tree != NULL); /* for lint */
      depth = SCIPtreeGetFocusDepth(tree);
      lpstateforkdepth = (tree->focuslpstatefork != NULL ? SCIPnodeGetDepth(tree->focuslpstatefork) : -1);
      
      SCIPdebugMessage("calling primal heuristics in depth %d (timing: %u)\n", depth, heurtiming);
   }

   /* call heuristics */
   ndelayedheurs = 0;
   oldnbestsolsfound = primal->nbestsolsfound;
   for( h = 0; h < set->nheurs; ++h )
   {
      /* it might happen that a diving heuristic renders the previously solved node LP invalid
       * such that additional calls to LP heuristics will fail; better abort the loop in this case
       */      
      if( lp != NULL && lp->resolvelperror) 
         break;

      SCIPdebugMessage(" -> executing heuristic <%s> with priority %d\n",
         SCIPheurGetName(set->heurs[h]), SCIPheurGetPriority(set->heurs[h]));
      SCIP_CALL( SCIPheurExec(set->heurs[h], set, primal, depth, lpstateforkdepth, heurtiming, &ndelayedheurs, &result) );

      /* make sure that heuristic did not leave on probing or diving mode */
      assert(tree == NULL || !SCIPtreeProbing(tree));
      assert(lp == NULL || !SCIPlpDiving(lp));
   }
   assert(0 <= ndelayedheurs && ndelayedheurs <= set->nheurs);

   *foundsol = (primal->nbestsolsfound > oldnbestsolsfound);

   return SCIP_OKAY;
}

/** applies one round of propagation */
static
SCIP_RETCODE propagationRound(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   int                   depth,              /**< depth level to use for propagator frequency checks */
   SCIP_Bool             fullpropagation,    /**< should all constraints be propagated (or only new ones)? */
   SCIP_Bool             onlydelayed,        /**< should only delayed propagators be called? */
   SCIP_Bool*            delayed,            /**< pointer to store whether a propagator was delayed */
   SCIP_Bool*            propagain,          /**< pointer to store whether propagation should be applied again */
   SCIP_Bool*            cutoff              /**< pointer to store whether the node can be cut off */
   )
{  /*lint --e{715}*/
   SCIP_RESULT result;
   SCIP_Bool abortoncutoff;
   int i;

   assert(set != NULL);
   assert(delayed != NULL);
   assert(propagain != NULL);
   assert(cutoff != NULL);

   *delayed = FALSE;
   *propagain = FALSE;

   /* sort propagators */
   SCIPsetSortProps(set);

   /* check if we want to abort on a cutoff; if we are not in the solving stage (e.g., in presolving), we want to abort
    * anyway
    */
   abortoncutoff = set->prop_abortoncutoff || (set->stage != SCIP_STAGE_SOLVING);
 
   /* call additional propagators with nonnegative priority */
   for( i = 0; i < set->nprops && (!(*cutoff) || !abortoncutoff); ++i )
   {
      if( SCIPpropGetPriority(set->props[i]) < 0 )
         continue;

      if( onlydelayed && !SCIPpropWasDelayed(set->props[i]) )
         continue;

      SCIP_CALL( SCIPpropExec(set->props[i], set, stat, depth, onlydelayed, &result) );
      *delayed = *delayed || (result == SCIP_DELAYED);
      *propagain = *propagain || (result == SCIP_REDUCEDDOM);
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      if( result == SCIP_CUTOFF )
      {
         SCIPdebugMessage(" -> propagator <%s> detected cutoff\n", SCIPpropGetName(set->props[i]));
      }

      /* if we work off the delayed propagators, we stop immediately if a reduction was found */
      if( onlydelayed && result == SCIP_REDUCEDDOM )
      {
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   /* propagate constraints */
   for( i = 0; i < set->nconshdlrs && (!(*cutoff) || !abortoncutoff); ++i )
   {
      if( onlydelayed && !SCIPconshdlrWasPropagationDelayed(set->conshdlrs[i]) )
         continue;

      SCIP_CALL( SCIPconshdlrPropagate(set->conshdlrs[i], blkmem, set, stat, depth, fullpropagation, onlydelayed,
            &result) );
      *delayed = *delayed || (result == SCIP_DELAYED);
      *propagain = *propagain || (result == SCIP_REDUCEDDOM);
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      if( result == SCIP_CUTOFF )
      {
         SCIPdebugMessage(" -> constraint handler <%s> detected cutoff in propagation\n",
            SCIPconshdlrGetName(set->conshdlrs[i]));
      }

      /* if we work off the delayed propagators, we stop immediately if a reduction was found */
      if( onlydelayed && result == SCIP_REDUCEDDOM )
      {
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   /* call additional propagators with negative priority */
   for( i = 0; i < set->nprops && (!(*cutoff) || !abortoncutoff); ++i )
   {
      if( SCIPpropGetPriority(set->props[i]) >= 0 )
         continue;

      if( onlydelayed && !SCIPpropWasDelayed(set->props[i]) )
         continue;

      SCIP_CALL( SCIPpropExec(set->props[i], set, stat, depth, onlydelayed, &result) );
      *delayed = *delayed || (result == SCIP_DELAYED);
      *propagain = *propagain || (result == SCIP_REDUCEDDOM);
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      if( result == SCIP_CUTOFF )
      {
         SCIPdebugMessage(" -> propagator <%s> detected cutoff\n", SCIPpropGetName(set->props[i]));
      }

      /* if we work off the delayed propagators, we stop immediately if a reduction was found */
      if( onlydelayed && result == SCIP_REDUCEDDOM )
      {
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   return SCIP_OKAY;
}

/** applies domain propagation on current node */
static
SCIP_RETCODE propagateDomains(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   int                   depth,              /**< depth level to use for propagator frequency checks */
   int                   maxproprounds,      /**< maximal number of propagation rounds (-1: no limit, 0: parameter settings) */
   SCIP_Bool             fullpropagation,    /**< should all constraints be propagated (or only new ones)? */
   SCIP_Bool*            cutoff              /**< pointer to store whether the node can be cut off */
   )
{
   SCIP_NODE* node;
   SCIP_Bool delayed;
   SCIP_Bool propagain;
   int propround;

   assert(set != NULL);
   assert(tree != NULL);
   assert(depth >= 0);
   assert(cutoff != NULL);

   node = SCIPtreeGetCurrentNode(tree);
   assert(node != NULL);
   assert(SCIPnodeIsActive(node));
   assert(SCIPnodeGetType(node) == SCIP_NODETYPE_FOCUSNODE
      || SCIPnodeGetType(node) == SCIP_NODETYPE_REFOCUSNODE
      || SCIPnodeGetType(node) == SCIP_NODETYPE_PROBINGNODE);

   /* adjust maximal number of propagation rounds */
   if( maxproprounds == 0 )
      maxproprounds = (depth == 0 ? set->prop_maxroundsroot : set->prop_maxrounds);
   if( maxproprounds == -1 )
      maxproprounds = INT_MAX;

   SCIPdebugMessage("domain propagation of node %p in depth %d (using depth %d, maxrounds %d)\n",
      (void*)node, SCIPnodeGetDepth(node), depth, maxproprounds);

   /* propagate as long new bound changes were found and the maximal number of propagation rounds is not exceeded */
   *cutoff = FALSE;
   propround = 0;
   propagain = TRUE;
   while( propagain && !(*cutoff) && propround < maxproprounds && !SCIPsolveIsStopped(set, stat, FALSE) )
   {
      propround++;

      /* perform the propagation round by calling the propagators and constraint handlers */
      SCIP_CALL( propagationRound(blkmem, set, stat, primal, tree, depth, fullpropagation, FALSE, &delayed, &propagain, cutoff) );

      /* if the propagation will be terminated, call the delayed propagators */
      while( delayed && (!propagain || propround >= maxproprounds) && !(*cutoff) )
      {
         /* call the delayed propagators and constraint handlers */
         SCIP_CALL( propagationRound(blkmem, set, stat, primal, tree, depth, fullpropagation, TRUE, &delayed, &propagain, cutoff) );
      }

      /* if a reduction was found, we want to do another full propagation round (even if the propagator only claimed
       * to have done a domain reduction without applying a domain change)
       */
      fullpropagation = TRUE;
   }

   /* mark the node to be completely propagated in the current repropagation subtree level */
   SCIPnodeMarkPropagated(node, tree);

   return SCIP_OKAY;
}

/** applies domain propagation on current node and flushes the conflict storage afterwards */
SCIP_RETCODE SCIPpropagateDomains(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_CONFLICT*        conflict,           /**< conflict analysis data */
   int                   depth,              /**< depth level to use for propagator frequency checks */
   int                   maxproprounds,      /**< maximal number of propagation rounds (-1: no limit, 0: parameter settings) */
   SCIP_Bool*            cutoff              /**< pointer to store whether the node can be cut off */
   )
{
   /* apply domain propagation */
   SCIP_CALL( propagateDomains(blkmem, set, stat, primal, tree, depth, maxproprounds, TRUE, cutoff) );

   /* flush the conflict set storage */
   SCIP_CALL( SCIPconflictFlushConss(conflict, blkmem, set, stat, prob, tree) );

   return SCIP_OKAY;
}

/** returns whether the given variable with the old LP solution value should lead to an update of the pseudo cost entry */
static
SCIP_Bool isPseudocostUpdateValid(
   SCIP_VAR*             var,                /**< problem variable */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_Real             oldlpsolval         /**< solution value of variable in old LP */
   )
{
   SCIP_Real newlpsolval;

   assert(var != NULL);

   /* if the old LP solution value is unknown, the pseudo cost update cannot be performed */
   if( oldlpsolval >= SCIP_INVALID )
      return FALSE;

   /* the bound change on the given variable was responsible for the gain in the dual bound, if the variable's
    * old solution value is outside the current bounds, and the new solution value is equal to the bound
    * closest to the old solution value
    */

   /* find out, which of the current bounds is violated by the old LP solution value */
   if( SCIPsetIsLT(set, oldlpsolval, SCIPvarGetLbLocal(var)) )
   {
      newlpsolval = SCIPvarGetLPSol(var);
      return SCIPsetIsEQ(set, newlpsolval, SCIPvarGetLbLocal(var));
   }
   else if( SCIPsetIsGT(set, oldlpsolval, SCIPvarGetUbLocal(var)) )
   {
      newlpsolval = SCIPvarGetLPSol(var);
      return SCIPsetIsEQ(set, newlpsolval, SCIPvarGetUbLocal(var));
   }
   else
      return FALSE;
}

/** pseudo cost flag stored in the variables to mark them for the pseudo cost update */
enum PseudocostFlag
{
   PSEUDOCOST_NONE     = 0,             /**< variable's bounds were not changed */
   PSEUDOCOST_IGNORE   = 1,             /**< bound changes on variable should be ignored for pseudo cost updates */
   PSEUDOCOST_UPDATE   = 2              /**< pseudo cost value of variable should be updated */
};
typedef enum PseudocostFlag PSEUDOCOSTFLAG;

/** updates the variable's pseudo cost values after the node's initial LP was solved */
static
SCIP_RETCODE updatePseudocost(
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp                  /**< LP data */
   )
{
   SCIP_NODE* focusnode;
   int actdepth;

   assert(lp != NULL);
   assert(tree != NULL);
   assert(tree->path != NULL);

   focusnode = SCIPtreeGetFocusNode(tree);
   assert(SCIPnodeIsActive(focusnode));
   assert(SCIPnodeGetType(focusnode) == SCIP_NODETYPE_FOCUSNODE);
   actdepth = SCIPnodeGetDepth(focusnode);
   assert(tree->path[actdepth] == focusnode);

   if( lp->solved && SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL && tree->focuslpstatefork != NULL )
   {
      SCIP_BOUNDCHG** updates;
      SCIP_NODE* node;
      SCIP_VAR* var;
      SCIP_Real weight;
      SCIP_Real lpgain;
      int nupdates;
      int nvalidupdates;
      int d;
      int i;

      assert(SCIPnodeIsActive(tree->focuslpstatefork));
      assert(tree->path[tree->focuslpstatefork->depth] == tree->focuslpstatefork);

      /* get a buffer for the collected bound changes; start with a size twice as large as the number of nodes between
       * current node and LP fork
       */
      SCIP_CALL( SCIPsetAllocBufferArray(set, &updates, 2*(actdepth - tree->focuslpstatefork->depth)) );
      nupdates = 0;
      nvalidupdates = 0;

      /* search the nodes from LP fork down to current node for bound changes in between; move in this direction,
       * because the bound changes closer to the LP fork are more likely to have a valid LP solution information
       * attached; collect the bound changes for pseudo cost value updates and mark the corresponding variables such
       * that they are not updated twice in case of more than one bound change on the same variable
       */
      for( d = tree->focuslpstatefork->depth+1; d <= actdepth; ++d )
      {
         node = tree->path[d];

         if( node->domchg != NULL )
         {
            SCIP_BOUNDCHG* boundchgs;
            int nboundchgs;

            boundchgs = node->domchg->domchgbound.boundchgs;
            nboundchgs = node->domchg->domchgbound.nboundchgs;
            for( i = 0; i < nboundchgs; ++i )
            {
               /* we even collect redundant bound changes, since they were not redundant in the LP branching decision
                * and therefore should be regarded in the pseudocost updates
                */
               if( (SCIP_BOUNDCHGTYPE)boundchgs[i].boundchgtype == SCIP_BOUNDCHGTYPE_BRANCHING )
               {
                  var = boundchgs[i].var;
                  assert(var != NULL);
                  if( (PSEUDOCOSTFLAG)var->pseudocostflag == PSEUDOCOST_NONE )
                  {
                     /* remember the bound change and mark the variable */
                     SCIP_CALL( SCIPsetReallocBufferArray(set, &updates, nupdates+1) );
                     updates[nupdates] = &boundchgs[i];
                     nupdates++;

                     /* check, if the bound change would lead to a valid pseudo cost update */
                     if( isPseudocostUpdateValid(var, set, boundchgs[i].data.branchingdata.lpsolval) )
                     {
                        var->pseudocostflag = PSEUDOCOST_UPDATE; /*lint !e641*/
                        nvalidupdates++;
                     }
                     else
                        var->pseudocostflag = PSEUDOCOST_IGNORE; /*lint !e641*/
                  }
               }
            }
         }
      }

      /* update the pseudo cost values and reset the variables' flags; assume, that the responsibility for the dual gain
       * is equally spread on all bound changes that lead to valid pseudo cost updates
       */
      weight = nvalidupdates > 0 ? 1.0 / (SCIP_Real)nvalidupdates : 1.0;
      lpgain = (SCIPlpGetObjval(lp, set) - tree->focuslpstatefork->lowerbound) * weight;
      lpgain = MAX(lpgain, 0.0);
      for( i = 0; i < nupdates; ++i )
      {
         assert((SCIP_BOUNDCHGTYPE)updates[i]->boundchgtype == SCIP_BOUNDCHGTYPE_BRANCHING);
         var = updates[i]->var;
         assert(var != NULL);
         assert((PSEUDOCOSTFLAG)var->pseudocostflag != PSEUDOCOST_NONE);
         if( (PSEUDOCOSTFLAG)var->pseudocostflag == PSEUDOCOST_UPDATE )
         {
            SCIPdebugMessage("updating pseudocosts of <%s>: sol: %g -> %g, LP: %e -> %e => gain=%g, weight: %g\n",
               SCIPvarGetName(var), updates[i]->data.branchingdata.lpsolval, SCIPvarGetLPSol(var),
               tree->focuslpstatefork->lowerbound, SCIPlpGetObjval(lp, set), lpgain, weight);
            SCIP_CALL( SCIPvarUpdatePseudocost(var, set, stat,
                  SCIPvarGetLPSol(var) - updates[i]->data.branchingdata.lpsolval, lpgain, weight) );
         }
         var->pseudocostflag = PSEUDOCOST_NONE; /*lint !e641*/
      }

      /* free the buffer for the collected bound changes */
      SCIPsetFreeBufferArray(set, &updates);
   }

   return SCIP_OKAY;
}

/** updates the estimated value of a primal feasible solution for the focus node after the LP was solved */
static
SCIP_RETCODE updateEstimate(
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< problem statistics */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< current LP data */
   SCIP_BRANCHCAND*      branchcand          /**< branching candidate storage */
   )
{
   SCIP_NODE* focusnode;
   SCIP_VAR** lpcands;
   SCIP_Real* lpcandsfrac;
   SCIP_Real estimate;
   int nlpcands;
   int i;

   assert(SCIPtreeHasFocusNodeLP(tree));

   /* estimate is only available if LP was solved to optimality */
   if( SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_OPTIMAL || !SCIPlpIsRelax(lp) )
      return SCIP_OKAY;

   focusnode = SCIPtreeGetFocusNode(tree);
   assert(focusnode != NULL);

   /* get the fractional variables */
   SCIP_CALL( SCIPbranchcandGetLPCands(branchcand, set, stat, lp, &lpcands, NULL, &lpcandsfrac, &nlpcands, NULL) );

   /* calculate the estimate: lowerbound + sum(min{f_j * pscdown_j, (1-f_j) * pscup_j}) */
   estimate = SCIPnodeGetLowerbound(focusnode);
   for( i = 0; i < nlpcands; ++i )
   {
      SCIP_Real pscdown;
      SCIP_Real pscup;

      pscdown = SCIPvarGetPseudocost(lpcands[i], stat, 0.0-lpcandsfrac[i]);
      pscup = SCIPvarGetPseudocost(lpcands[i], stat, 1.0-lpcandsfrac[i]);
      estimate += MIN(pscdown, pscup);
   }
   SCIPnodeSetEstimate(focusnode, stat, estimate);

   return SCIP_OKAY;
}

/** puts all constraints with initial flag TRUE into the LP */
static
SCIP_RETCODE initConssLP(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_EVENTFILTER*     eventfilter,        /**< global event filter */
   SCIP_Bool             root,               /**< is this the initial root LP? */
   SCIP_Bool*            cutoff              /**< pointer to store whether the node can be cut off */
   )
{
   int h;

   assert(set != NULL);
   assert(lp != NULL);
   assert(cutoff != NULL);
   
   /* inform separation storage, that LP is now filled with initial data */
   SCIPsepastoreStartInitialLP(sepastore);

   /* add LP relaxations of all initial constraints to LP */
   SCIPdebugMessage("init LP: initial rows\n");
   for( h = 0; h < set->nconshdlrs; ++h )
   {
      SCIP_CALL( SCIPconshdlrInitLP(set->conshdlrs[h], blkmem, set, stat) );
   }
   SCIP_CALL( SCIPsepastoreApplyCuts(sepastore, blkmem, set, stat, tree, lp, branchcand, eventqueue, eventfilter, root, cutoff) );

   /* inform separation storage, that initial LP setup is now finished */
   SCIPsepastoreEndInitialLP(sepastore);

  return SCIP_OKAY;
}

/** constructs the initial LP of the current node */
static
SCIP_RETCODE initLP(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_PRICESTORE*      pricestore,         /**< pricing storage */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_EVENTFILTER*     eventfilter,        /**< global event filter */
   SCIP_Bool             root,               /**< is this the initial root LP? */
   SCIP_Bool*            cutoff              /**< pointer to store whether the node can be cut off */
   )
{
   SCIP_VAR* var;
   int v;

   assert(set != NULL);
   assert(prob != NULL);
   assert(lp != NULL);
   assert(cutoff != NULL);

   *cutoff = FALSE;

   /* at the root node, we have to add the initial variables as columns */
   if( root )
   {
      assert(SCIPlpGetNCols(lp) == 0);
      assert(SCIPlpGetNRows(lp) == 0);
      assert(lp->nremovablecols == 0);
      assert(lp->nremovablerows == 0);

      /* inform pricing storage, that LP is now filled with initial data */
      SCIPpricestoreStartInitialLP(pricestore);

      /* add all initial variables to LP */
      SCIPdebugMessage("init LP: initial columns\n");
      for( v = 0; v < prob->nvars; ++v )
      {
         var = prob->vars[v];
         assert(SCIPvarGetProbindex(var) >= 0);

         if( SCIPvarIsInitial(var) )
         {
            SCIP_CALL( SCIPpricestoreAddVar(pricestore, blkmem, set, eventqueue, lp, var, 0.0, TRUE) );
         }
      }
      assert(lp->nremovablecols == 0);
      SCIP_CALL( SCIPpricestoreApplyVars(pricestore, blkmem, set, stat, eventqueue, prob, tree, lp) );

      /* inform pricing storage, that initial LP setup is now finished */
      SCIPpricestoreEndInitialLP(pricestore);
   }

   /* put all initial constraints into the LP */
   SCIP_CALL( initConssLP(blkmem, set, sepastore, stat, tree, lp, branchcand, eventqueue, eventfilter, root, cutoff) );

   return SCIP_OKAY;
}

/** constructs the LP of the current node, but does not load the LP state and warmstart information  */
SCIP_RETCODE SCIPconstructCurrentLP(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_PRICESTORE*      pricestore,         /**< pricing storage */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_EVENTFILTER*     eventfilter,        /**< global event filter */
   SCIP_Bool*            cutoff              /**< pointer to store whether the node can be cut off */
   )
{
   SCIP_Bool initroot;

   assert(tree != NULL);
   assert(cutoff != NULL);

   *cutoff = FALSE;

   if( !SCIPtreeIsFocusNodeLPConstructed(tree) )
   {
      /* load the LP into the solver and load the LP state */
      SCIPdebugMessage("loading LP\n");
      SCIP_CALL( SCIPtreeLoadLP(tree, blkmem, set, eventqueue, eventfilter, lp, &initroot) );
      assert(initroot || SCIPnodeGetDepth(SCIPtreeGetFocusNode(tree)) > 0);
      assert(SCIPtreeIsFocusNodeLPConstructed(tree));

      /* setup initial LP relaxation of node */
      SCIP_CALL( initLP(blkmem, set, stat, prob, tree, lp, pricestore, sepastore, branchcand, eventqueue, eventfilter, initroot,
            cutoff) );
   }

   return SCIP_OKAY;
}

/** load and solve the initial LP of a node */
static
SCIP_RETCODE solveNodeInitialLP(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_PRICESTORE*      pricestore,         /**< pricing storage */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_EVENTFILTER*     eventfilter,        /**< event filter for global (not variable dependent) events */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_Bool*            cutoff,             /**< pointer to store whether the node can be cut off */
   SCIP_Bool*            lperror             /**< pointer to store whether an unresolved error in LP solving occured */
   )
{
   assert(stat != NULL);
   assert(tree != NULL);
   assert(lp != NULL);
   assert(cutoff != NULL);
   assert(lperror != NULL);
   assert(SCIPtreeGetFocusNode(tree) != NULL);
   assert(SCIPnodeGetType(SCIPtreeGetFocusNode(tree)) == SCIP_NODETYPE_FOCUSNODE);

   *cutoff = FALSE;
   *lperror = FALSE;

   /* load the LP into the solver */
   SCIP_CALL( SCIPconstructCurrentLP(blkmem, set, stat, prob, tree, lp, pricestore, sepastore, branchcand, eventqueue,
         eventfilter, cutoff) );
   if( *cutoff )
      return SCIP_OKAY;

   /* load the LP state */
   SCIP_CALL( SCIPtreeLoadLPState(tree, blkmem, set, stat, eventqueue, lp) );

   /* solve initial LP */
   SCIPdebugMessage("node: solve initial LP\n");
   SCIP_CALL( SCIPlpSolveAndEval(lp, blkmem, set, stat, eventqueue, eventfilter, prob, -1, TRUE, FALSE, lperror) );
   assert(lp->flushed);
   assert(lp->solved || *lperror);

   if( !(*lperror) )
   {
      SCIP_EVENT event;

      if( SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_ITERLIMIT && SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_TIMELIMIT )
      {         
         /* issue FIRSTLPSOLVED event */
         SCIP_CALL( SCIPeventChgType(&event, SCIP_EVENTTYPE_FIRSTLPSOLVED) );
         SCIP_CALL( SCIPeventChgNode(&event, SCIPtreeGetFocusNode(tree)) );
         SCIP_CALL( SCIPeventProcess(&event, set, NULL, NULL, NULL, eventfilter) );
      }
      
      /* update pseudo cost values */
      SCIP_CALL( updatePseudocost(set, stat, tree, lp) );
   }

   return SCIP_OKAY;
}

/** makes sure the LP is flushed and solved */
static
SCIP_RETCODE separationRoundResolveLP(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_EVENTFILTER*     eventfilter,        /**< global event filter */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_Bool*            cutoff,             /**< pointer to store whether the node can be cut off */
   SCIP_Bool*            lperror,            /**< pointer to store whether an unresolved error in LP solving occured */
   SCIP_Bool*            mustsepa,           /**< pointer to store TRUE if additional separation rounds should be performed */
   SCIP_Bool*            mustprice           /**< pointer to store TRUE if additional pricing rounds should be performed */
   )
{
   assert(lp != NULL);
   assert(cutoff != NULL);
   assert(lperror != NULL);
   assert(mustsepa != NULL);
   assert(mustprice != NULL);

   /* if bound changes were applied in the separation round, we have to resolve the LP */
   if( !(*cutoff) && !lp->flushed )
   {
      /* solve LP (with dual simplex) */
      SCIPdebugMessage("separation: resolve LP\n");

      SCIP_CALL( SCIPlpSolveAndEval(lp, blkmem, set, stat, eventqueue, eventfilter, prob, -1, TRUE, FALSE, lperror) );
      assert(lp->flushed);
      assert(lp->solved || *lperror);
      *mustsepa = TRUE;
      *mustprice = TRUE;
   }

   return SCIP_OKAY;
}

/** applies one round of LP separation */
static
SCIP_RETCODE separationRoundLP(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_EVENTFILTER*     eventfilter,        /**< global event filter */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   int                   actdepth,           /**< current depth in the tree */
   SCIP_Real             bounddist,          /**< current relative distance of local dual bound to global dual bound */
   SCIP_Bool             onlydelayed,        /**< should only delayed separators be called? */
   SCIP_Bool*            delayed,            /**< pointer to store whether a separator was delayed */
   SCIP_Bool*            enoughcuts,         /**< pointer to store whether enough cuts have been found this round */
   SCIP_Bool*            cutoff,             /**< pointer to store whether the node can be cut off */
   SCIP_Bool*            lperror,            /**< pointer to store whether an unresolved error in LP solving occured */
   SCIP_Bool*            mustsepa,           /**< pointer to store TRUE if additional separation rounds should be performed */
   SCIP_Bool*            mustprice           /**< pointer to store TRUE if additional pricing rounds should be performed */
   )
{
   SCIP_RESULT result;
   int i;
   SCIP_Bool consadded;
   SCIP_Bool root;

   assert(set != NULL);
   assert(lp != NULL);
   assert(set->conshdlrs_sepa != NULL);
   assert(delayed != NULL);
   assert(enoughcuts != NULL);
   assert(cutoff != NULL);
   assert(lperror != NULL);

   root = (actdepth == 0);
   *delayed = FALSE;
   *enoughcuts = (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
   *lperror = FALSE;
   consadded = FALSE;
   
   SCIPdebugMessage("calling separators on LP solution in depth %d (onlydelayed: %u)\n", actdepth, onlydelayed);

   /* sort separators by priority */
   SCIPsetSortSepas(set);

   /* call LP separators with nonnegative priority */
   for( i = 0; i < set->nsepas && !(*cutoff) && !(*lperror) && !(*enoughcuts) && lp->flushed && lp->solved
           && (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY);
        ++i )
   {
      if( SCIPsepaGetPriority(set->sepas[i]) < 0 )
         continue;

      if( onlydelayed && !SCIPsepaWasLPDelayed(set->sepas[i]) )
         continue;

      SCIPdebugMessage(" -> executing separator <%s> with priority %d\n",
         SCIPsepaGetName(set->sepas[i]), SCIPsepaGetPriority(set->sepas[i]));
      SCIP_CALL( SCIPsepaExecLP(set->sepas[i], set, stat, sepastore, actdepth, bounddist, onlydelayed, &result) );
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      consadded = consadded || (result == SCIP_CONSADDED);
      *enoughcuts = *enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
      *delayed = *delayed || (result == SCIP_DELAYED);
      if( *cutoff )
      {
         SCIPdebugMessage(" -> separator <%s> detected cutoff\n", SCIPsepaGetName(set->sepas[i]));
      }

      /* make sure the LP is solved (after adding bound changes, LP has to be flushed and resolved) */
      SCIP_CALL( separationRoundResolveLP(blkmem, set, stat, eventqueue, eventfilter, prob, lp, cutoff, lperror, mustsepa, mustprice) );

      /* if we work off the delayed separators, we stop immediately if a cut was found */
      if( onlydelayed && (result == SCIP_CONSADDED || result == SCIP_REDUCEDDOM || result == SCIP_SEPARATED) )
      {
         SCIPdebugMessage(" -> delayed separator <%s> found a cut\n", SCIPsepaGetName(set->sepas[i]));
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   /* try separating constraints of the constraint handlers */
   for( i = 0; i < set->nconshdlrs && !(*cutoff) && !(*lperror) && !(*enoughcuts) && lp->flushed && lp->solved
           && (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY);
        ++i )
   {
      if( onlydelayed && !SCIPconshdlrWasLPSeparationDelayed(set->conshdlrs_sepa[i]) )
         continue;

      SCIPdebugMessage(" -> executing separation of constraint handler <%s> with priority %d\n",
         SCIPconshdlrGetName(set->conshdlrs_sepa[i]), SCIPconshdlrGetSepaPriority(set->conshdlrs_sepa[i]));
      SCIP_CALL( SCIPconshdlrSeparateLP(set->conshdlrs_sepa[i], blkmem, set, stat, sepastore, actdepth, onlydelayed,
            &result) );
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      consadded = consadded || (result == SCIP_CONSADDED);
      *enoughcuts = *enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
      *delayed = *delayed || (result == SCIP_DELAYED);
      if( *cutoff )
      {
         SCIPdebugMessage(" -> constraint handler <%s> detected cutoff in separation\n",
            SCIPconshdlrGetName(set->conshdlrs_sepa[i]));
      }

      /* make sure the LP is solved (after adding bound changes, LP has to be flushed and resolved) */
      SCIP_CALL( separationRoundResolveLP(blkmem, set, stat, eventqueue, eventfilter, prob, lp, cutoff, lperror, mustsepa, mustprice) );

      /* if we work off the delayed separators, we stop immediately if a cut was found */
      if( onlydelayed && (result == SCIP_CONSADDED || result == SCIP_REDUCEDDOM || result == SCIP_SEPARATED) )
      {
         SCIPdebugMessage(" -> delayed constraint handler <%s> found a cut\n",
            SCIPconshdlrGetName(set->conshdlrs_sepa[i]));
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   /* call LP separators with negative priority */
   for( i = 0; i < set->nsepas && !(*cutoff) && !(*lperror) && !(*enoughcuts) && lp->flushed && lp->solved
           && (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY);
        ++i )
   {
      if( SCIPsepaGetPriority(set->sepas[i]) >= 0 )
         continue;

      if( onlydelayed && !SCIPsepaWasLPDelayed(set->sepas[i]) )
         continue;

      SCIPdebugMessage(" -> executing separator <%s> with priority %d\n",
         SCIPsepaGetName(set->sepas[i]), SCIPsepaGetPriority(set->sepas[i]));
      SCIP_CALL( SCIPsepaExecLP(set->sepas[i], set, stat, sepastore, actdepth, bounddist, onlydelayed, &result) );
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      consadded = consadded || (result == SCIP_CONSADDED);
      *enoughcuts = *enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
      *delayed = *delayed || (result == SCIP_DELAYED);
      if( *cutoff )
      {
         SCIPdebugMessage(" -> separator <%s> detected cutoff\n", SCIPsepaGetName(set->sepas[i]));
      }

      /* make sure the LP is solved (after adding bound changes, LP has to be flushed and resolved) */
      SCIP_CALL( separationRoundResolveLP(blkmem, set, stat, eventqueue, eventfilter, prob, lp, cutoff, lperror, mustsepa, mustprice) );

      /* if we work off the delayed separators, we stop immediately if a cut was found */
      if( onlydelayed && (result == SCIP_CONSADDED || result == SCIP_REDUCEDDOM || result == SCIP_SEPARATED) )
      {
         SCIPdebugMessage(" -> delayed separator <%s> found a cut\n", SCIPsepaGetName(set->sepas[i]));
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   /* process the constraints that were added during this separation round */
   while( consadded )
   {
      assert(!onlydelayed);
      consadded = FALSE;

      for( i = 0; i < set->nconshdlrs && !(*cutoff) && !(*lperror) && !(*enoughcuts) && lp->flushed && lp->solved
              && (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY);
           ++i )
      {
	 SCIPdebugMessage(" -> executing separation of constraint handler <%s> with priority %d\n",
            SCIPconshdlrGetName(set->conshdlrs_sepa[i]), SCIPconshdlrGetSepaPriority(set->conshdlrs_sepa[i]));
	 SCIP_CALL( SCIPconshdlrSeparateLP(set->conshdlrs_sepa[i], blkmem, set, stat, sepastore, actdepth, onlydelayed,
               &result) );
	 *cutoff = *cutoff || (result == SCIP_CUTOFF);
	 consadded = consadded || (result == SCIP_CONSADDED);
	 *enoughcuts = *enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
	 *delayed = *delayed || (result == SCIP_DELAYED);
         if( *cutoff )
         {
            SCIPdebugMessage(" -> constraint handler <%s> detected cutoff in separation\n",
               SCIPconshdlrGetName(set->conshdlrs_sepa[i]));
         }

         /* make sure the LP is solved (after adding bound changes, LP has to be flushed and resolved) */
         SCIP_CALL( separationRoundResolveLP(blkmem, set, stat, eventqueue, eventfilter, prob, lp, cutoff, lperror, mustsepa, mustprice) );
      }
   }

   SCIPdebugMessage(" -> separation round finished: delayed=%u, enoughcuts=%u, lpflushed=%u, cutoff=%u\n",
      *delayed, *enoughcuts, lp->flushed, *cutoff);

   return SCIP_OKAY;
}

/** applies one round of separation on the given primal solution */
static
SCIP_RETCODE separationRoundSol(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_SOL*             sol,                /**< primal solution that should be separated, or NULL for LP solution */
   int                   actdepth,           /**< current depth in the tree */
   SCIP_Bool             onlydelayed,        /**< should only delayed separators be called? */
   SCIP_Bool*            delayed,            /**< pointer to store whether a separator was delayed */
   SCIP_Bool*            enoughcuts,         /**< pointer to store whether enough cuts have been found this round */
   SCIP_Bool*            cutoff              /**< pointer to store whether the node can be cut off */
   )
{
   SCIP_RESULT result;
   int i;
   SCIP_Bool consadded;
   SCIP_Bool root;

   assert(set != NULL);
   assert(set->conshdlrs_sepa != NULL);
   assert(delayed != NULL);
   assert(enoughcuts != NULL);
   assert(cutoff != NULL);

   *delayed = FALSE;
   *enoughcuts = FALSE;
   consadded = FALSE;
   root = (actdepth == 0);

   SCIPdebugMessage("calling separators on primal solution in depth %d (onlydelayed: %u)\n", actdepth, onlydelayed);

   /* sort separators by priority */
   SCIPsetSortSepas(set);

   /* call separators with nonnegative priority */
   for( i = 0; i < set->nsepas && !(*cutoff) && !(*enoughcuts) && !SCIPsolveIsStopped(set, stat, FALSE); ++i )
   {
      if( SCIPsepaGetPriority(set->sepas[i]) < 0 )
         continue;

      if( onlydelayed && !SCIPsepaWasSolDelayed(set->sepas[i]) )
         continue;

      SCIP_CALL( SCIPsepaExecSol(set->sepas[i], set, stat, sepastore, sol, actdepth, onlydelayed, &result) );
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      consadded = consadded || (result == SCIP_CONSADDED);
      *enoughcuts = *enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
      *delayed = *delayed || (result == SCIP_DELAYED);
      if( *cutoff )
      {
         SCIPdebugMessage(" -> separator <%s> detected cutoff\n", SCIPsepaGetName(set->sepas[i]));
      }

      /* if we work off the delayed separators, we stop immediately if a cut was found */
      if( onlydelayed && (result == SCIP_CONSADDED || result == SCIP_REDUCEDDOM || result == SCIP_SEPARATED) )
      {
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   /* try separating constraints of the constraint handlers */
   for( i = 0; i < set->nconshdlrs && !(*cutoff) && !(*enoughcuts) && !SCIPsolveIsStopped(set, stat, FALSE); ++i )
   {
      if( onlydelayed && !SCIPconshdlrWasSolSeparationDelayed(set->conshdlrs_sepa[i]) )
         continue;

      SCIP_CALL( SCIPconshdlrSeparateSol(set->conshdlrs_sepa[i], blkmem, set, stat, sepastore, sol, actdepth, onlydelayed,
            &result) );
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      consadded = consadded || (result == SCIP_CONSADDED);
      *enoughcuts = *enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
      *delayed = *delayed || (result == SCIP_DELAYED);
      if( *cutoff )
      {
         SCIPdebugMessage(" -> constraint handler <%s> detected cutoff in separation\n",
            SCIPconshdlrGetName(set->conshdlrs_sepa[i]));
      }

      /* if we work off the delayed separators, we stop immediately if a cut was found */
      if( onlydelayed && (result == SCIP_CONSADDED || result == SCIP_REDUCEDDOM || result == SCIP_SEPARATED) )
      {
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   /* call separators with negative priority */
   for( i = 0; i < set->nsepas && !(*cutoff) && !(*enoughcuts) && !SCIPsolveIsStopped(set, stat, FALSE); ++i )
   {
      if( SCIPsepaGetPriority(set->sepas[i]) >= 0 )
         continue;

      if( onlydelayed && !SCIPsepaWasSolDelayed(set->sepas[i]) )
         continue;

      SCIP_CALL( SCIPsepaExecSol(set->sepas[i], set, stat, sepastore, sol, actdepth, onlydelayed, &result) );
      *cutoff = *cutoff || (result == SCIP_CUTOFF);
      consadded = consadded || (result == SCIP_CONSADDED);
      *enoughcuts = *enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
      *delayed = *delayed || (result == SCIP_DELAYED);
      if( *cutoff )
      {
         SCIPdebugMessage(" -> separator <%s> detected cutoff\n", SCIPsepaGetName(set->sepas[i]));
      }

      /* if we work off the delayed separators, we stop immediately if a cut was found */
      if( onlydelayed && (result == SCIP_CONSADDED || result == SCIP_REDUCEDDOM || result == SCIP_SEPARATED) )
      {
         *delayed = TRUE;
         return SCIP_OKAY;
      }
   }

   /* process the constraints that were added during this separation round */
   while( consadded )
   {
      assert(!onlydelayed);
      consadded = FALSE;

      for( i = 0; i < set->nconshdlrs && !(*cutoff) && !(*enoughcuts) && !SCIPsolveIsStopped(set, stat, FALSE); ++i )
      {
	 SCIP_CALL( SCIPconshdlrSeparateSol(set->conshdlrs_sepa[i], blkmem, set, stat, sepastore, sol, actdepth, onlydelayed, &result) );
	 *cutoff = *cutoff || (result == SCIP_CUTOFF);
	 consadded = consadded || (result == SCIP_CONSADDED);
	 *enoughcuts = *enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
	 *delayed = *delayed || (result == SCIP_DELAYED);
         if( *cutoff )
         {
            SCIPdebugMessage(" -> constraint handler <%s> detected cutoff in separation\n",
               SCIPconshdlrGetName(set->conshdlrs_sepa[i]));
         }
      }
   }

   SCIPdebugMessage(" -> separation round finished: delayed=%u, enoughcuts=%u, cutoff=%u\n",
      *delayed, *enoughcuts, *cutoff);

   return SCIP_OKAY;
}

/** applies one round of separation on the given primal solution or on the LP solution */
SCIP_RETCODE SCIPseparationRound(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_EVENTFILTER*     eventfilter,        /**< global event filter */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_SOL*             sol,                /**< primal solution that should be separated, or NULL for LP solution */
   int                   actdepth,           /**< current depth in the tree */
   SCIP_Bool             onlydelayed,        /**< should only delayed separators be called? */
   SCIP_Bool*            delayed,            /**< pointer to store whether a separator was delayed */
   SCIP_Bool*            cutoff              /**< pointer to store whether the node can be cut off */
   )
{
   SCIP_Bool enoughcuts;

   assert(delayed != NULL);
   assert(cutoff != NULL);

   *delayed = FALSE;
   *cutoff = FALSE;
   enoughcuts = FALSE;

   if( sol == NULL )
   {
      SCIP_Bool lperror;
      SCIP_Bool mustsepa;
      SCIP_Bool mustprice;

      /* apply a separation round on the LP solution */
      lperror = FALSE;
      mustsepa = FALSE;
      mustprice = FALSE;
      SCIP_CALL( separationRoundLP(blkmem, set, stat, eventqueue, eventfilter, prob, lp, sepastore, actdepth, 0.0, onlydelayed, delayed, &enoughcuts,
            cutoff, &lperror, &mustsepa, &mustprice) );
   }
   else
   {
      /* apply a separation round on the given primal solution */
      SCIP_CALL( separationRoundSol(blkmem, set, stat, sepastore, sol, actdepth, onlydelayed, delayed, &enoughcuts, cutoff) );
   }

   return SCIP_OKAY;
}

/** solves the current LP completely with pricing in new variables */
SCIP_RETCODE SCIPpriceLoop(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_PRICESTORE*      pricestore,         /**< pricing storage */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_EVENTFILTER*     eventfilter,        /**< global event filter */
   SCIP_Bool             pretendroot,        /**< should the pricers be called as if we are at the root node? */
   SCIP_Bool             displayinfo,        /**< should info lines be displayed after each pricing round? */
   int                   maxpricerounds,     /**< maximal number of pricing rounds (-1: no limit);
                                              *   a finite limit means that the LP might not be solved to optimality! */
   int*                  npricedcolvars,     /**< pointer to store number of column variables after problem vars were priced */
   SCIP_Bool*            mustsepa,           /**< pointer to store TRUE if a separation round should follow */
   SCIP_Real*            lowerbound,         /**< lower bound computed by the pricers */
   SCIP_Bool*            lperror,            /**< pointer to store whether an unresolved error in LP solving occured */
   SCIP_Bool*            aborted             /**< pointer to store whether the pricing was aborted and the lower bound must 
                                              *   not be used */
   )
{
   int npricerounds;
   SCIP_Bool mustprice;
   SCIP_Bool cutoff;

   assert(prob != NULL);
   assert(lp != NULL);
   assert(lp->flushed);
   assert(lp->solved);
   assert(npricedcolvars != NULL);
   assert(mustsepa != NULL);
   assert(lperror != NULL);
   assert(lowerbound != NULL);
   assert(aborted != NULL);

   *npricedcolvars = prob->ncolvars;
   *lperror = FALSE;
   *aborted = FALSE;

   /* if the LP is unbounded, we don't need to price */
   mustprice = (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL 
      || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_INFEASIBLE 
      || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OBJLIMIT);

   /* if all the variables are already in the LP, we don't need to price */
   mustprice = mustprice && !SCIPprobAllColsInLP(prob, set, lp);

   /* check if infinite number of pricing rounds should be used */
   if( maxpricerounds == -1 )
      maxpricerounds = INT_MAX;

   /* pricing (has to be done completely to get a valid lower bound) */
   npricerounds = 0;
   while( !(*lperror) && mustprice && npricerounds < maxpricerounds )
   {
      SCIP_Bool enoughvars;
      SCIP_RESULT result;
      SCIP_Real lb;
      SCIP_Bool foundsol;
      int p;

      assert(lp->flushed);
      assert(lp->solved);
      assert(SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_UNBOUNDEDRAY);

      /* check if pricing loop should be aborted */
      if( SCIPsolveIsStopped(set, stat, FALSE) )
      {
         SCIPwarningMessage("pricing has been interrupted -- LP of current node is invalid\n");
         *aborted = TRUE;
         break;
      }

      /* call primal heuristics which are callable during pricing */
      SCIP_CALL( SCIPprimalHeuristics(set, stat, primal, tree, lp, NULL, SCIP_HEURTIMING_DURINGPRICINGLOOP, &foundsol) );

      /* price problem variables */
      SCIPdebugMessage("problem variable pricing\n");
      assert(SCIPpricestoreGetNVars(pricestore) == 0);
      assert(SCIPpricestoreGetNBoundResets(pricestore) == 0);
      SCIP_CALL( SCIPpricestoreAddProbVars(pricestore, blkmem, set, stat, prob, tree, lp, branchcand, eventqueue) );
      *npricedcolvars = prob->ncolvars;

      /* call external pricers to create additional problem variables */
      SCIPdebugMessage("external variable pricing\n");

      /* sort pricer algorithms by priority */
      SCIPsetSortPricers(set);

      /* call external pricer algorithms, that are active for the current problem */
      enoughvars = (SCIPpricestoreGetNVars(pricestore) >= SCIPsetGetPriceMaxvars(set, pretendroot)/2 + 1);
      for( p = 0; p < set->nactivepricers && !enoughvars; ++p )
      {
         SCIP_CALL( SCIPpricerExec(set->pricers[p], set, prob, lp, pricestore, &lb, &result) );
         assert(result == SCIP_DIDNOTRUN || result == SCIP_SUCCESS);
         SCIPdebugMessage("pricing: pricer %s returned result = %s, lowerbound = %f\n", 
            SCIPpricerGetName(set->pricers[p]), (result == SCIP_DIDNOTRUN ? "didnotrun" : "success"), lb);
         enoughvars = enoughvars || (SCIPpricestoreGetNVars(pricestore) >= (SCIPsetGetPriceMaxvars(set, pretendroot)+1)/2);
         *aborted = ( (*aborted) || (result == SCIP_DIDNOTRUN) );
         *lowerbound = MAX(*lowerbound, lb);
      }

      /* apply the priced variables to the LP */
      SCIP_CALL( SCIPpricestoreApplyVars(pricestore, blkmem, set, stat, eventqueue, prob, tree, lp) );
      assert(SCIPpricestoreGetNVars(pricestore) == 0);
      assert(!lp->flushed || lp->solved);
      mustprice = !lp->flushed || (prob->ncolvars != *npricedcolvars);
      *mustsepa = *mustsepa || !lp->flushed;

      /* after adding columns, the LP should be primal feasible such that primal simplex is applicable;
       * if LP was infeasible, we have to use dual simplex
       */
      SCIPdebugMessage("pricing: solve LP\n");
      SCIP_CALL( SCIPlpSolveAndEval(lp, blkmem, set, stat, eventqueue, eventfilter, prob, -1, TRUE, FALSE, lperror) );
      assert(lp->flushed);
      assert(lp->solved || *lperror);

      /* reset bounds temporarily set by pricer to their original values */
      SCIPdebugMessage("pricing: reset bounds\n");
      SCIP_CALL( SCIPpricestoreResetBounds(pricestore, blkmem, set, stat, lp, branchcand, eventqueue) );
      assert(SCIPpricestoreGetNVars(pricestore) == 0);
      assert(SCIPpricestoreGetNBoundResets(pricestore) == 0);
      assert(!lp->flushed || lp->solved || *lperror);

      /* put all initial constraints into the LP */
      SCIP_CALL( initConssLP(blkmem, set, sepastore, stat, tree, lp, branchcand, eventqueue, eventfilter, pretendroot, &cutoff) );
      assert(cutoff == FALSE);

      mustprice = mustprice || !lp->flushed || (prob->ncolvars != *npricedcolvars);
      *mustsepa = *mustsepa || !lp->flushed;
      
      /* solve LP again after resetting bounds and adding new initial constraints (with dual simplex) */
      SCIPdebugMessage("pricing: solve LP after resetting bounds and adding new initial constraints\n");
      SCIP_CALL( SCIPlpSolveAndEval(lp, blkmem, set, stat, eventqueue, eventfilter, prob, -1, FALSE, FALSE, lperror) );
      assert(lp->flushed);
      assert(lp->solved || *lperror);

      /* increase pricing round counter */
      stat->npricerounds++;
      npricerounds++;

      /* display node information line */
      if( displayinfo && mustprice )
      {
         if( (SCIP_VERBLEVEL)set->disp_verblevel >= SCIP_VERBLEVEL_FULL
             || ((SCIP_VERBLEVEL)set->disp_verblevel >= SCIP_VERBLEVEL_HIGH && npricerounds % 100 == 1) )
         {
            SCIP_CALL( SCIPdispPrintLine(set, stat, NULL, TRUE) );
         }
      }

      /* if the LP is unbounded, we can stop pricing */
      mustprice = mustprice && 
         (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL 
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_INFEASIBLE
          || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OBJLIMIT );

   }
   assert(lp->flushed);
   assert(lp->solved || *lperror);

   *aborted = ( (*aborted) || (*lperror) || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_NOTSOLVED 
      || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_ERROR || npricerounds == maxpricerounds );

   /* set information, whether the current lp is a valid relaxation of the current problem */
   SCIPlpSetIsRelax(lp, !(*aborted));

   return SCIP_OKAY;
}

/** solve the current LP of a node with a price-and-cut loop */
static
SCIP_RETCODE priceAndCutLoop(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_PRICESTORE*      pricestore,         /**< pricing storage */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_CUTPOOL*         cutpool,            /**< global cut pool */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_CONFLICT*        conflict,           /**< conflict analysis data */
   SCIP_EVENTFILTER*     eventfilter,        /**< event filter for global (not variable dependent) events */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_Bool             initiallpsolved,    /**< was the initial LP already solved? */
   SCIP_Bool*            cutoff,             /**< pointer to store whether the node can be cut off */
   SCIP_Bool*            unbounded,          /**< pointer to store whether an unbounded ray was found in the LP */
   SCIP_Bool*            lperror,            /**< pointer to store whether an unresolved error in LP solving occured */
   SCIP_Bool*            pricingaborted      /**< pointer to store whether the pricing was aborted and the lower bound must 
                                              *   not be used */ 
   )
{
   SCIP_NODE* focusnode;
   SCIP_RESULT result;
   SCIP_EVENT event;
   SCIP_Real loclowerbound;
   SCIP_Real glblowerbound;
   SCIP_Real pricerlowerbound;
   SCIP_Real bounddist;
   SCIP_Real stalllpobjval;
   SCIP_Bool separate;
   SCIP_Bool mustprice;
   SCIP_Bool mustsepa;
   SCIP_Bool delayedsepa;
   SCIP_Bool root;
   int maxseparounds;
   int nsepastallrounds;
   int maxnsepastallrounds;
   int stallnfracs;
   int actdepth;
   int npricedcolvars;

   assert(set != NULL);
   assert(blkmem != NULL);
   assert(stat != NULL);
   assert(prob != NULL);
   assert(tree != NULL);
   assert(lp != NULL);
   assert(pricestore != NULL);
   assert(sepastore != NULL);
   assert(cutpool != NULL);
   assert(primal != NULL);
   assert(cutoff != NULL);
   assert(unbounded != NULL);
   assert(lperror != NULL);

   focusnode = SCIPtreeGetFocusNode(tree);
   assert(focusnode != NULL);
   assert(SCIPnodeGetType(focusnode) == SCIP_NODETYPE_FOCUSNODE);
   actdepth = SCIPnodeGetDepth(focusnode);
   root = (actdepth == 0);

   /* check, if we want to separate at this node */
   loclowerbound = SCIPnodeGetLowerbound(focusnode);
   glblowerbound = SCIPtreeGetLowerbound(tree, set);
   assert(primal->cutoffbound > glblowerbound);
   bounddist = (loclowerbound - glblowerbound)/(primal->cutoffbound - glblowerbound);
   separate = SCIPsetIsLE(set, bounddist, set->sepa_maxbounddist);
   separate = separate && (set->sepa_maxruns == -1 || stat->nruns <= set->sepa_maxruns);

   /* get maximal number of separation rounds */
   maxseparounds = (root ? set->sepa_maxroundsroot : set->sepa_maxrounds);
   if( maxseparounds == -1 )
      maxseparounds = INT_MAX;
   if( stat->nruns > 1 && root && set->sepa_maxroundsrootsubrun >= 0 )
      maxseparounds = MIN(maxseparounds, set->sepa_maxroundsrootsubrun);
   if( initiallpsolved && set->sepa_maxaddrounds >= 0 )
      maxseparounds = MIN(maxseparounds, stat->nseparounds + set->sepa_maxaddrounds);
   maxnsepastallrounds = set->sepa_maxstallrounds;
   if( maxnsepastallrounds == -1 )
      maxnsepastallrounds = INT_MAX;

   /* solve initial LP of price-and-cut loop */
   SCIPdebugMessage("node: solve LP with price and cut\n");
   SCIP_CALL( SCIPlpSolveAndEval(lp, blkmem, set, stat, eventqueue, eventfilter, prob, -1, TRUE, FALSE, lperror) );
   assert(lp->flushed);
   assert(lp->solved || *lperror);

   /* price-and-cut loop */
   npricedcolvars = prob->ncolvars;
   mustprice = TRUE;
   mustsepa = separate;
   delayedsepa = FALSE;
   *cutoff = FALSE;
   *unbounded = FALSE;
   nsepastallrounds = 0;
   stalllpobjval = SCIP_REAL_MIN;
   stallnfracs = INT_MAX;
   lp->installing = FALSE;
   while( !(*cutoff) && !(*lperror) && (mustprice || mustsepa || delayedsepa) )
   {
      SCIPdebugMessage("-------- node solving loop --------\n");
      assert(lp->flushed);
      assert(lp->solved);

      /* solve the LP with pricing in new variables */
      while( mustprice && !(*lperror) )
      {
         SCIP_Real oldlowerbound;

         oldlowerbound = SCIPtreeGetLowerbound(tree, set);

         pricerlowerbound = -SCIPsetInfinity(set);

         SCIP_CALL( SCIPpriceLoop(blkmem, set, stat, prob, primal, tree, lp, pricestore, sepastore, branchcand, eventqueue,
               eventfilter, root, root, -1, &npricedcolvars, &mustsepa, &pricerlowerbound, lperror, pricingaborted) );

         mustprice = FALSE;

         /* update lower bound w.r.t. the lower bound given by the pricers */
         SCIPnodeUpdateLowerbound(focusnode, stat, pricerlowerbound);
         SCIPdebugMessage(" -> new lower bound given by pricers: %g\n", pricerlowerbound);

         assert(lp->flushed);
         assert(lp->solved || *lperror);

         /* update lower bound w.r.t. the LP solution */
         if( !(*lperror) && !(*pricingaborted))
         {
            SCIP_CALL( SCIPnodeUpdateLowerboundLP(focusnode, set, stat, lp) );
            SCIPdebugMessage(" -> new lower bound: %g (LP status: %d, LP obj: %g)\n",
                             SCIPnodeGetLowerbound(focusnode), SCIPlpGetSolstat(lp), SCIPlpGetObjval(lp, set));

            /* update node estimate */
            SCIP_CALL( updateEstimate(set, stat, tree, lp, branchcand) );
         }
         else
         {
            SCIPdebugMessage(" -> error solving LP or pricing aborted. keeping old bound: %g\n", SCIPnodeGetLowerbound(focusnode));
         }

         /* display node information line for root node */
         if( root && (SCIP_VERBLEVEL)set->disp_verblevel >= SCIP_VERBLEVEL_HIGH )
         {
            SCIP_CALL( SCIPdispPrintLine(set, stat, NULL, TRUE) );
         }

         if( !(*lperror) )
         {
            SCIP_Real newlowerbound;

            /* if the global lower bound changed, propagate domains again since this may trigger reductions 
             * propagation only has to be performed if the node is not cut off by bounding anyway 
             */
            newlowerbound = SCIPtreeGetLowerbound(tree, set);
            if( SCIPsetIsGT(set, newlowerbound, oldlowerbound) && SCIPsetIsLT(set, SCIPnodeGetLowerbound(focusnode), primal->cutoffbound) )
            {
               SCIPdebugMessage(" -> global lower bound changed from %g to %g: propagate domains again\n",
                                oldlowerbound, newlowerbound);
               SCIP_CALL( propagateDomains(blkmem, set, stat, primal, tree, SCIPtreeGetCurrentDepth(tree), 0, FALSE, cutoff) );
               assert(SCIPbufferGetNUsed(set->buffer) == 0);

               /* if we found something, solve LP again */
               if( !lp->flushed && !(*cutoff) )
               {
                  SCIPdebugMessage("    -> found reduction: resolve LP\n");

                  /* in the root node, remove redundant rows permanently from the LP */
                  if( root )
                  {
                     SCIP_CALL( SCIPlpFlush(lp, blkmem, set, eventqueue) );
                     SCIP_CALL( SCIPlpRemoveRedundantRows(lp, blkmem, set, stat, eventqueue, eventfilter) );
                  }

                  /* resolve LP */
                  SCIP_CALL( SCIPlpSolveAndEval(lp, blkmem, set, stat, eventqueue, eventfilter, prob, -1, TRUE, FALSE, lperror) );
                  assert(lp->flushed);
                  assert(lp->solved || *lperror);

                  mustprice = TRUE;
               }
            }
         }

         /* call primal heuristics that are applicable during node LP solving loop */
         if( SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL )
         {
            SCIP_Bool foundsol;

            SCIP_CALL( SCIPprimalHeuristics(set, stat, primal, tree, lp, NULL, SCIP_HEURTIMING_DURINGLPLOOP, &foundsol) );
            assert(SCIPbufferGetNUsed(set->buffer) == 0);

            *lperror = *lperror || lp->resolvelperror;
         }
      }
      assert(lp->flushed || *cutoff);
      assert(lp->solved || *lperror || *cutoff);

      /* check, if we exceeded the separation round limit */
      mustsepa = mustsepa
         && stat->nseparounds < maxseparounds
         && nsepastallrounds < maxnsepastallrounds
         && !(*cutoff);

      /* if separators were delayed, we want to apply a final separation round with the delayed separators */
      delayedsepa = delayedsepa && !mustsepa && !(*cutoff); /* if regular separation applies, we ignore delayed separators */
      mustsepa = mustsepa || delayedsepa;

      /* if the LP is infeasible, exceeded the objective limit or a global performance limit was reached, 
       * we don't need to separate cuts
       * (the global limits are only checked at the root node in order to not query system time too often)
       */
      if( mustsepa )
      {
         if( !separate
             || (SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_OPTIMAL && SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_UNBOUNDEDRAY)
             || SCIPsetIsGE(set, SCIPnodeGetLowerbound(focusnode), primal->cutoffbound)
             || (root && SCIPsolveIsStopped(set, stat, FALSE)) )
         {
            mustsepa = FALSE;
            delayedsepa = FALSE;
         }
      }

      /* separation and reduced cost strengthening
       * (needs not to be done completely, because we just want to increase the lower bound)
       */
      if( !(*cutoff) && !(*lperror) && mustsepa )
      {
         SCIP_Longint olddomchgcount;
         SCIP_Bool enoughcuts;

         assert(lp->flushed);
         assert(lp->solved);
         assert(SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY);

         olddomchgcount = stat->domchgcount;

         mustsepa = FALSE;
         enoughcuts = (SCIPsetGetSepaMaxcuts(set, root) == 0);

         /* global cut pool separation */
         if( !enoughcuts && !delayedsepa )
         {
            if( (set->sepa_poolfreq == 0 && actdepth == 0)
               || (set->sepa_poolfreq > 0 && actdepth % set->sepa_poolfreq == 0) )
            {
               SCIPdebugMessage("global cut pool separation\n");
               assert(SCIPsepastoreGetNCuts(sepastore) == 0);
               SCIP_CALL( SCIPcutpoolSeparate(cutpool, blkmem, set, stat, eventqueue, eventfilter, lp, sepastore, root, &result) );
               *cutoff = *cutoff || (result == SCIP_CUTOFF);
               enoughcuts = enoughcuts || (SCIPsepastoreGetNCuts(sepastore) >= 2 * (SCIP_Longint)SCIPsetGetSepaMaxcuts(set, root));
               if( *cutoff )
               {
                  SCIPdebugMessage(" -> global cut pool detected cutoff\n");
               }
            }
         }
         assert(lp->flushed);
         assert(lp->solved);
         assert(SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY);

         /* constraint separation */
         SCIPdebugMessage("constraint separation\n");

         /* separate constraints and LP */
         if( !(*cutoff) && !(*lperror) && !enoughcuts && lp->solved
            && (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY) )
         {
            /* apply a separation round */
            SCIP_CALL( separationRoundLP(blkmem, set, stat, eventqueue, eventfilter, prob, lp, sepastore, actdepth, bounddist, delayedsepa,
                  &delayedsepa, &enoughcuts, cutoff, lperror, &mustsepa, &mustprice) );
            assert(SCIPbufferGetNUsed(set->buffer) == 0);

            /* if we are close to the stall round limit, also call the delayed separators */
            if( !(*cutoff) && !(*lperror) && !enoughcuts && lp->solved
               && (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY)
               && nsepastallrounds >= maxnsepastallrounds-1 && delayedsepa )
            {
               SCIP_CALL( separationRoundLP(blkmem, set, stat, eventqueue, eventfilter, prob, lp, sepastore, actdepth, bounddist, delayedsepa,
                     &delayedsepa, &enoughcuts, cutoff, lperror, &mustsepa, &mustprice) );
               assert(SCIPbufferGetNUsed(set->buffer) == 0);
            }
         }
         assert(*cutoff || *lperror || SCIPlpIsSolved(lp));
         assert(!SCIPlpIsSolved(lp)
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_INFEASIBLE
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OBJLIMIT
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_ITERLIMIT
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_TIMELIMIT);

         if( *cutoff || *lperror
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_INFEASIBLE || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OBJLIMIT 
            || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_ITERLIMIT  || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_TIMELIMIT )
         {
            /* the found cuts are of no use, because the node is infeasible anyway (or we have an error in the LP) */
            SCIP_CALL( SCIPsepastoreClearCuts(sepastore, blkmem, set, eventqueue, eventfilter, lp) );
         }
         else
         {
            /* apply found cuts */
            SCIP_CALL( SCIPsepastoreApplyCuts(sepastore, blkmem, set, stat, tree, lp, branchcand, eventqueue, eventfilter, root,
                  cutoff) );

            if( !(*cutoff) )
            {
               mustprice = mustprice || !lp->flushed || (prob->ncolvars != npricedcolvars);
               mustsepa = mustsepa || !lp->flushed;

               /* if a new bound change (e.g. a cut with only one column) was found, propagate domains again */
               if( stat->domchgcount != olddomchgcount )
               {
                  /* propagate domains */
                  SCIP_CALL( propagateDomains(blkmem, set, stat, primal, tree, SCIPtreeGetCurrentDepth(tree), 0, FALSE, cutoff) );
                  assert(SCIPbufferGetNUsed(set->buffer) == 0);

                  /* in the root node, remove redundant rows permanently from the LP */
                  if( root )
                  {
                     SCIP_CALL( SCIPlpFlush(lp, blkmem, set, eventqueue) );
                     SCIP_CALL( SCIPlpRemoveRedundantRows(lp, blkmem, set, stat, eventqueue, eventfilter) );
                  }
               }

               if( !(*cutoff) )
               {
                  SCIP_Real lpobjval;

                  /* solve LP (with dual simplex) */
                  SCIPdebugMessage("separation: solve LP\n");
                  SCIP_CALL( SCIPlpSolveAndEval(lp, blkmem, set, stat, eventqueue, eventfilter, prob, -1, TRUE, FALSE, lperror) );
                  assert(lp->flushed);
                  assert(lp->solved || *lperror);

                  if( !(*lperror) && SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL )
                  {
                     SCIP_Real objreldiff;
                     int nfracs;

                     if( SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY )
                     {
                        SCIP_CALL( SCIPbranchcandGetLPCands(branchcand, set, stat, lp, NULL, NULL, NULL, &nfracs, NULL) );
                     }
                     else
                        nfracs = INT_MAX;
                     lpobjval = SCIPlpGetObjval(lp, set);
                     objreldiff = SCIPrelDiff(lpobjval, stalllpobjval);
                     SCIPdebugMessage(" -> LP bound moved from %g to %g (reldiff: %g)\n",
                        stalllpobjval, lpobjval, objreldiff);
                     if( objreldiff > 1e-04 || nfracs <= (0.9 - 0.1 * nsepastallrounds) * stallnfracs )
                     {
                        nsepastallrounds = 0;
                        stalllpobjval = lpobjval;
                        stallnfracs = nfracs;
                        lp->installing = FALSE;
                     }
                     else
                        nsepastallrounds++;
                     /* tell LP that we are (close to) stalling */
                     if ( nsepastallrounds >= maxnsepastallrounds-2 )
                        lp->installing = TRUE;
                     SCIPdebugMessage(" -> nsepastallrounds=%d/%d\n", nsepastallrounds, maxnsepastallrounds);
                  }
               }
            }
         }
         assert(*cutoff || *lperror || (lp->flushed && lp->solved)); /* cutoff: LP may be unsolved due to bound changes */

         SCIPdebugMessage("separation round %d/%d finished (%d/%d stall rounds): mustprice=%u, mustsepa=%u, delayedsepa=%u\n",
            stat->nseparounds, maxseparounds, nsepastallrounds, maxnsepastallrounds, mustprice, mustsepa, delayedsepa);

         /* increase separation round counter */
         stat->nseparounds++;
      }
   }

   /* update lower bound w.r.t. the LP solution */
   if( *cutoff )
   {
      SCIPnodeUpdateLowerbound(focusnode, stat, SCIPsetInfinity(set));
   }
   else if( !(*lperror) )
   {
      assert(lp->flushed);
      assert(lp->solved);

      SCIP_CALL( SCIPnodeUpdateLowerboundLP(focusnode, set, stat, lp) );

      /* update node estimate */
      SCIP_CALL( updateEstimate(set, stat, tree, lp, branchcand) );

      /* issue LPSOLVED event */
      if( SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_ITERLIMIT && SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_TIMELIMIT )
      {         
         SCIP_CALL( SCIPeventChgType(&event, SCIP_EVENTTYPE_LPSOLVED) );
         SCIP_CALL( SCIPeventChgNode(&event, focusnode) );
         SCIP_CALL( SCIPeventProcess(&event, set, NULL, NULL, NULL, eventfilter) );
      }

      /* analyze an infeasible LP (not necessary in the root node) */
      if( !set->misc_exactsolve && !root && SCIPlpIsRelax(lp)
         && (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_INFEASIBLE || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OBJLIMIT) )
      {
         SCIP_CALL( SCIPconflictAnalyzeLP(conflict, blkmem, set, stat, prob, tree, lp, NULL) );
      }

      /* check for unboundness */
      if( SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY )
      {
         assert(root); /* this can only happen in the root node */
         *unbounded = TRUE;
      }
   }
   lp->installing = FALSE;

   SCIPdebugMessage(" -> final lower bound: %g (LP status: %d, LP obj: %g)\n",
      SCIPnodeGetLowerbound(focusnode), SCIPlpGetSolstat(lp),
      *cutoff ? SCIPsetInfinity(set) : *lperror ? -SCIPsetInfinity(set) : SCIPlpGetObjval(lp, set));

   return SCIP_OKAY;
}

/** updates the current lower bound with the pseudo objective value, cuts off node by bounding, and applies conflict
 *  analysis if the pseudo objective lead to the cutoff
 */
static
SCIP_RETCODE applyBounding(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_CONFLICT*        conflict,           /**< conflict analysis data */
   SCIP_Bool*            cutoff              /**< pointer to store TRUE, if the node can be cut off */
   )
{
   assert(primal != NULL);
   assert(cutoff != NULL);

   if( !(*cutoff) )
   {
      SCIP_NODE* focusnode;
      SCIP_Real pseudoobjval;

      /* get current focus node */
      focusnode = SCIPtreeGetFocusNode(tree);

      /* update lower bound w.r.t. the pseudo solution */
      pseudoobjval = SCIPlpGetPseudoObjval(lp, set);
      SCIPnodeUpdateLowerbound(focusnode, stat, pseudoobjval);
      SCIPdebugMessage(" -> lower bound: %g [%g] (pseudoobj: %g [%g]), cutoff bound: %g [%g]\n",
         SCIPnodeGetLowerbound(focusnode), SCIPprobExternObjval(prob, set, SCIPnodeGetLowerbound(focusnode)),
         pseudoobjval, SCIPprobExternObjval(prob, set, pseudoobjval),
         primal->cutoffbound, SCIPprobExternObjval(prob, set, primal->cutoffbound));

      /* check for infeasible node by bounding */
      if( (set->misc_exactsolve && SCIPnodeGetLowerbound(focusnode) >= primal->cutoffbound)
         || (!set->misc_exactsolve && SCIPsetIsGE(set, SCIPnodeGetLowerbound(focusnode), primal->cutoffbound)) )
      {
         SCIPdebugMessage("node is cut off by bounding (lower=%g, upper=%g)\n",
            SCIPnodeGetLowerbound(focusnode), primal->cutoffbound);
         SCIPnodeUpdateLowerbound(focusnode, stat, SCIPsetInfinity(set));
         *cutoff = TRUE;

         /* call pseudo conflict analysis, if the node is cut off due to the pseudo objective value */
         if( pseudoobjval >= primal->cutoffbound && !SCIPsetIsInfinity(set, -pseudoobjval) )
         {
            SCIP_CALL( SCIPconflictAnalyzePseudo(conflict, blkmem, set, stat, prob, tree, lp, NULL) );
         }
      }
   }

   return SCIP_OKAY;
}

/** solves the current node's LP in a price-and-cut loop */
static
SCIP_RETCODE solveNodeLP(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_PRICESTORE*      pricestore,         /**< pricing storage */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_CUTPOOL*         cutpool,            /**< global cut pool */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_CONFLICT*        conflict,           /**< conflict analysis data */
   SCIP_EVENTFILTER*     eventfilter,        /**< event filter for global (not variable dependent) events */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_Bool             initiallpsolved,    /**< was the initial LP already solved? */
   SCIP_Bool*            cutoff,             /**< pointer to store TRUE, if the node can be cut off */
   SCIP_Bool*            unbounded,          /**< pointer to store TRUE, if an unbounded ray was found in the LP */
   SCIP_Bool*            lperror,            /**< pointer to store TRUE, if an unresolved error in LP solving occured */
   SCIP_Bool*            pricingaborted      /**< pointer to store TRUE, if the pricing was aborted and the lower bound must not be used */ 
   )
{
   SCIP_Longint nlpiterations;
   int nlps;

   assert(stat != NULL);
   assert(tree != NULL);
   assert(SCIPtreeHasFocusNodeLP(tree));
   assert(cutoff != NULL);
   assert(unbounded != NULL);
   assert(lperror != NULL);
   assert(*cutoff == FALSE);
   assert(*unbounded == FALSE);
   assert(*lperror == FALSE);

   nlps = stat->nlps;
   nlpiterations = stat->nlpiterations;

   if( !initiallpsolved )
   {
      /* load and solve the initial LP of the node */
      SCIP_CALL( solveNodeInitialLP(blkmem, set, stat, prob, tree, lp, pricestore, sepastore,
            branchcand, eventfilter, eventqueue, cutoff, lperror) );
      assert(*cutoff || *lperror || (lp->flushed && lp->solved));
      SCIPdebugMessage("price-and-cut-loop: initial LP status: %d, LP obj: %g\n",
         SCIPlpGetSolstat(lp),
         *cutoff ? SCIPsetInfinity(set) : *lperror ? -SCIPsetInfinity(set) : SCIPlpGetObjval(lp, set));

      /* update initial LP iteration counter */
      stat->ninitlps += stat->nlps - nlps;
      stat->ninitlpiterations += stat->nlpiterations - nlpiterations;

      /* in the root node, we try if initial LP solution is feasible to avoid expensive setup of data structures in
       * separators; in case the root LP is aborted, e.g, by hitting the time limit, we do not check the LP solution
       * since the corresponding data structures have not been updated 
       */
      if( SCIPtreeGetCurrentDepth(tree) == 0 && !(*cutoff) && !(*lperror)
         && (SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY)
         && !SCIPsolveIsStopped(set, stat, FALSE) )
      {
         SCIP_Bool checklprows;
         SCIP_Bool stored;
         SCIP_SOL* sol;
         
         SCIP_CALL( SCIPsolCreateLPSol(&sol, blkmem, set, stat, primal, tree, lp, NULL) );

         if( SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY )
            checklprows = FALSE;
         else
            checklprows = TRUE;

#ifndef NDEBUG
         /* in the debug mode we want to explizitly check if the solution is feasible if it was stored */
         SCIP_CALL( SCIPprimalTrySol(primal, blkmem, set, stat, prob, tree, lp, eventfilter, sol, FALSE, TRUE, TRUE, 
               checklprows, &stored) );

         if( stored )
         {
            SCIP_Bool feasible;

            SCIP_CALL( SCIPsolCheck(sol, blkmem, set, stat, prob, FALSE, TRUE, TRUE, checklprows, &feasible) );
            assert(feasible);
         }

         SCIP_CALL( SCIPsolFree(&sol, blkmem, primal) );
#else
         SCIP_CALL( SCIPprimalTrySolFree(primal, blkmem, set, stat, prob, tree, lp, eventfilter, &sol, FALSE, TRUE, TRUE, 
               checklprows, &stored) );
#endif    
         /* if the solution was accepted, the root node can be cut off by bounding */
         if( stored && SCIPprobAllColsInLP(prob, set, lp) )
         {
            SCIPdebugMessage("root node initial LP feasible --> cut off root node, stop solution process\n");
            SCIP_CALL( SCIPnodeUpdateLowerboundLP(SCIPtreeGetFocusNode(tree), set, stat, lp) );
            SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );
            assert(*cutoff);
         }
         if( SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY )
            *unbounded = TRUE;
      }
   }
   assert(SCIPsepastoreGetNCuts(sepastore) == 0);

   if( !(*cutoff) && !(*lperror) )
   {
      /* solve the LP with price-and-cut*/
      SCIP_CALL( priceAndCutLoop(blkmem, set, stat, prob, primal, tree, lp, pricestore, sepastore, cutpool,
            branchcand, conflict, eventfilter, eventqueue, initiallpsolved, cutoff, unbounded, lperror, pricingaborted) );
   }
   assert(*cutoff || *lperror || (lp->flushed && lp->solved));

   /* If pricing was aborted while solving the LP of the node and the node can not be cut off due to the lower bound computed by the pricer,
   *  the solving of the LP might be stopped due to the objective limit, but the node may not be cut off, since the LP objective
   *  is not a feasible lower bound for the solutions in the current subtree. 
   *  In this case, the LP has to be solved to optimality by temporarily removing the cutoff bound. 
   */
   if ( (*pricingaborted) && SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OBJLIMIT && !(*cutoff) )
   {
      SCIP_Real tmpcutoff;
      
      /* temporarily disable cutoffbound, which also disables the objective limit */ 
      tmpcutoff = lp->cutoffbound;
      lp->cutoffbound = SCIPlpiInfinity(SCIPlpGetLPI(lp));

      lp->solved = FALSE;
      SCIP_CALL( SCIPlpSolveAndEval(lp, blkmem, set, stat, eventqueue, eventfilter, prob, -1, FALSE, FALSE, lperror) );
      
      /* reinstall old cutoff bound */
      lp->cutoffbound = tmpcutoff;

      SCIPdebugMessage("re-optimized LP without cutoff bound: LP status: %d, LP obj: %g\n",
         SCIPlpGetSolstat(lp), *lperror ? -SCIPsetInfinity(set) : SCIPlpGetObjval(lp, set));

      /* lp solstat should not be objlimit, since the cutoff bound was removed temporarily */
      assert(SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_OBJLIMIT);
      /* lp solstat should not be unboundedray, since the lp was dual feasible */
      assert(SCIPlpGetSolstat(lp) != SCIP_LPSOLSTAT_UNBOUNDEDRAY);
      if ( SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_INFEASIBLE )
      {
         *cutoff = TRUE;
      }
   }
   assert(!(*pricingaborted) || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || (*cutoff));

   assert(*cutoff || *lperror || (lp->flushed && lp->solved));

   /* update node's LP iteration counter */
   stat->nnodelps += stat->nlps - nlps;
   stat->nnodelpiterations += stat->nlpiterations - nlpiterations;

   /* update number of root node iterations if the root node was processed */
   if( SCIPnodeGetDepth(tree->focusnode) == 0 ) 
      stat->nrootlpiterations += stat->nlpiterations - nlpiterations;

   return SCIP_OKAY;
}

/** calls relaxators */
static
SCIP_RETCODE solveNodeRelax(
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   int                   depth,              /**< depth of current node */
   SCIP_Bool             beforelp,           /**< should the relaxators with non-negative or negative priority be called? */
   SCIP_Bool*            cutoff,             /**< pointer to store TRUE, if the node can be cut off */
   SCIP_Bool*            propagateagain,     /**< pointer to store TRUE, if domain propagation should be applied again */
   SCIP_Bool*            solvelpagain,       /**< pointer to store TRUE, if the node's LP has to be solved again */
   SCIP_Bool*            solverelaxagain     /**< pointer to store TRUE, if the external relaxators should be called
                                              *   again */
   )
{
   SCIP_RESULT result;
   SCIP_Real lowerbound;
   int r;

   assert(set != NULL);
   assert(cutoff != NULL);
   assert(solvelpagain != NULL);
   assert(propagateagain != NULL);
   assert(solverelaxagain != NULL);
   assert(!(*cutoff));

   /* sort by priority */
   SCIPsetSortRelaxs(set);

   for( r = 0; r < set->nrelaxs && !(*cutoff); ++r )
   {
      if( beforelp != (SCIPrelaxGetPriority(set->relaxs[r]) >= 0) )
         continue;

      lowerbound = -SCIPsetInfinity(set);

      SCIP_CALL( SCIPrelaxExec(set->relaxs[r], set, stat, depth, &lowerbound, &result) );

      switch( result )
      {
      case SCIP_CUTOFF:
         *cutoff = TRUE;
         SCIPdebugMessage(" -> relaxator <%s> detected cutoff\n", SCIPrelaxGetName(set->relaxs[r]));
         break;

      case SCIP_CONSADDED:
         *solvelpagain = TRUE;   /* the separation for new constraints should be called */
         *propagateagain = TRUE; /* the propagation for new constraints should be called */
         break;

      case SCIP_REDUCEDDOM:
         *solvelpagain = TRUE;
         *propagateagain = TRUE;
         break;

      case SCIP_SEPARATED:
         *solvelpagain = TRUE;
         break;

      case SCIP_SUSPENDED:
         *solverelaxagain = TRUE;
         break;

      case SCIP_SUCCESS:
      case SCIP_DIDNOTRUN:
         break;

      default:
         SCIPerrorMessage("invalid result code <%d> of relaxator <%s>\n", result, SCIPrelaxGetName(set->relaxs[r]));
         return SCIP_INVALIDRESULT;
      }  /*lint !e788*/

      if( result != SCIP_CUTOFF && result != SCIP_DIDNOTRUN && result != SCIP_SUSPENDED )
      {
         SCIP_NODE* focusnode;
         
         focusnode = SCIPtreeGetFocusNode(tree);
         assert(focusnode != NULL);
         assert(SCIPnodeGetType(focusnode) == SCIP_NODETYPE_FOCUSNODE);
         
         /* update lower bound w.r.t. the lower bound given by the relaxator */
         SCIPnodeUpdateLowerbound(focusnode, stat, lowerbound);
         SCIPdebugMessage(" -> new lower bound given by relaxator %s: %g\n", 
            SCIPrelaxGetName(set->relaxs[r]), lowerbound);
      }
   }

   return SCIP_OKAY;
}

/** marks all relaxators to be unsolved */
static
void markRelaxsUnsolved(
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_RELAXATION*      relaxation          /**< global relaxation data */
   )
{
   int r;

   assert(set != NULL);
   assert(relaxation != NULL);

   SCIPrelaxationSetSolValid(relaxation, FALSE);

   for( r = 0; r < set->nrelaxs; ++r )
      SCIPrelaxMarkUnsolved(set->relaxs[r]);
}

/** enforces constraints by branching, separation, or domain reduction */
static
SCIP_RETCODE enforceConstraints(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_RELAXATION*      relaxation,         /**< global relaxation data */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_Bool*            branched,           /**< pointer to store whether a branching was created */
   SCIP_Bool*            cutoff,             /**< pointer to store TRUE, if the node can be cut off */
   SCIP_Bool*            infeasible,         /**< pointer to store TRUE, if the LP/pseudo solution is infeasible */
   SCIP_Bool*            propagateagain,     /**< pointer to store TRUE, if domain propagation should be applied again */
   SCIP_Bool*            solvelpagain,       /**< pointer to store TRUE, if the node's LP has to be solved again */
   SCIP_Bool*            solverelaxagain,    /**< pointer to store TRUE, if the external relaxators should be called again */
   SCIP_Bool             forced              /**< should enforcement of pseudo solution be forced? */
   )
{
   SCIP_RESULT result;
   SCIP_Real pseudoobjval;
   SCIP_Bool resolved;
   SCIP_Bool objinfeasible;
   int h;

   assert(set != NULL);
   assert(stat != NULL);
   assert(tree != NULL);
   assert(SCIPtreeGetFocusNode(tree) != NULL);
   assert(branched != NULL);
   assert(cutoff != NULL);
   assert(infeasible != NULL);
   assert(propagateagain != NULL);
   assert(solvelpagain != NULL);
   assert(solverelaxagain != NULL);
   assert(!(*cutoff));
   assert(!(*propagateagain));
   assert(!(*solvelpagain));
   assert(!(*solverelaxagain));

   *branched = FALSE;
   /**@todo avoid checking the same pseudosolution twice */

   /* enforce constraints by branching, applying additional cutting planes (if LP is being processed),
    * introducing new constraints, or tighten the domains
    */
   SCIPdebugMessage("enforcing constraints on %s solution\n", SCIPtreeHasFocusNodeLP(tree) ? "LP" : "pseudo");

   /* check, if the solution is infeasible anyway due to it's objective value */
   if( SCIPtreeHasFocusNodeLP(tree) )
      objinfeasible = FALSE;
   else
   {
      pseudoobjval = SCIPlpGetPseudoObjval(lp, set);
      objinfeasible = SCIPsetIsLT(set, pseudoobjval, SCIPnodeGetLowerbound(SCIPtreeGetFocusNode(tree)));
   }

   /* during constraint enforcement, generated cuts should enter the LP in any case; otherwise, a constraint handler
    * would fail to enforce its constraints if it relies on the modification of the LP relaxation
    */
   SCIPsepastoreStartForceCuts(sepastore);

   /* enforce constraints until a handler resolved an infeasibility with cutting off the node, branching,
    * reducing a domain, or separating a cut
    * if a constraint handler introduced new constraints to enforce his constraints, the newly added constraints
    * have to be enforced themselves
    */
   resolved = FALSE;
   for( h = 0; h < set->nconshdlrs && !resolved; ++h )
   {
      assert(SCIPsepastoreGetNCuts(sepastore) == 0); /* otherwise, the LP should have been resolved first */

      if( SCIPtreeHasFocusNodeLP(tree) )
      { 
         assert(lp->flushed);
         assert(lp->solved);
         assert(SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OPTIMAL || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_UNBOUNDEDRAY);
         SCIP_CALL( SCIPconshdlrEnforceLPSol(set->conshdlrs_enfo[h], blkmem, set, stat, tree, sepastore, *infeasible,
               &result) );
      }
      else
      {
         SCIP_CALL( SCIPconshdlrEnforcePseudoSol(set->conshdlrs_enfo[h], blkmem, set, stat, tree, branchcand, *infeasible,
               objinfeasible,forced, &result) );
         if( SCIPsepastoreGetNCuts(sepastore) != 0 )
         {
            SCIPerrorMessage("pseudo enforcing method of constraint handler <%s> separated cuts\n",
               SCIPconshdlrGetName(set->conshdlrs_enfo[h]));
            return SCIP_INVALIDRESULT;
         }
      }
      SCIPdebugMessage("enforcing of <%s> returned result %d\n", SCIPconshdlrGetName(set->conshdlrs_enfo[h]), result);

      switch( result )
      {
      case SCIP_CUTOFF:
         assert(tree->nchildren == 0);
         *cutoff = TRUE;
         *infeasible = TRUE;
         resolved = TRUE;
         SCIPdebugMessage(" -> constraint handler <%s> detected cutoff in enforcement\n",
            SCIPconshdlrGetName(set->conshdlrs_enfo[h]));
         break;

      case SCIP_CONSADDED:
         assert(tree->nchildren == 0);
         *infeasible = TRUE;
         *propagateagain = TRUE; /* the propagation for new constraints should be called */
         *solvelpagain = TRUE;   /* the separation for new constraints should be called */
         *solverelaxagain = TRUE; 
         markRelaxsUnsolved(set, relaxation);
         resolved = TRUE;
         break;

      case SCIP_REDUCEDDOM:
         assert(tree->nchildren == 0);
         *infeasible = TRUE;
         *propagateagain = TRUE;
         *solvelpagain = TRUE;
         *solverelaxagain = TRUE;
         markRelaxsUnsolved(set, relaxation);
         resolved = TRUE;
         break;

      case SCIP_SEPARATED:
         assert(tree->nchildren == 0);
         assert(SCIPsepastoreGetNCuts(sepastore) > 0);
         *infeasible = TRUE;
         *solvelpagain = TRUE;
         *solverelaxagain = TRUE;
         markRelaxsUnsolved(set, relaxation);
         resolved = TRUE;
         break;

      case SCIP_BRANCHED:
         assert(tree->nchildren >= 1);
         assert(!SCIPtreeHasFocusNodeLP(tree) || (lp->flushed && lp->solved));
         assert(SCIPsepastoreGetNCuts(sepastore) == 0);
         *infeasible = TRUE;
         *branched = TRUE;
         resolved = TRUE;
         break;

      case SCIP_SOLVELP:
         assert(!SCIPtreeHasFocusNodeLP(tree));
         assert(tree->nchildren == 0);
         assert(SCIPsepastoreGetNCuts(sepastore) == 0);
         *infeasible = TRUE;
         *solvelpagain = TRUE;
         resolved = TRUE;
         SCIPtreeSetFocusNodeLP(tree, TRUE); /* the node's LP must be solved */
         break;

      case SCIP_INFEASIBLE:
         assert(tree->nchildren == 0);
         assert(!SCIPtreeHasFocusNodeLP(tree) || (lp->flushed && lp->solved));
         assert(SCIPsepastoreGetNCuts(sepastore) == 0);
         *infeasible = TRUE;
         break;

      case SCIP_FEASIBLE:
         assert(tree->nchildren == 0);
         assert(!SCIPtreeHasFocusNodeLP(tree) || (lp->flushed && lp->solved));
         assert(SCIPsepastoreGetNCuts(sepastore) == 0);
         break;

      case SCIP_DIDNOTRUN:
         assert(tree->nchildren == 0);
         assert(!SCIPtreeHasFocusNodeLP(tree) || (lp->flushed && lp->solved));
         assert(SCIPsepastoreGetNCuts(sepastore) == 0);
         assert(objinfeasible);
         *infeasible = TRUE;
         break;

      default:
         SCIPerrorMessage("invalid result code <%d> from enforcing method of constraint handler <%s>\n",
            result, SCIPconshdlrGetName(set->conshdlrs_enfo[h]));
         return SCIP_INVALIDRESULT;
      }  /*lint !e788*/

      /* the enforcement method may add a primal solution, after which the LP status could be set to
       * objective limit reached
       */
      if( SCIPtreeHasFocusNodeLP(tree) && SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_OBJLIMIT )
      {
         *cutoff = TRUE;
         *infeasible = TRUE;
         resolved = TRUE;
         SCIPdebugMessage(" -> LP exceeded objective limit\n");
      }

      assert(!(*branched) || (resolved && !(*cutoff) && *infeasible && !(*propagateagain) && !(*solvelpagain)));
      assert(!(*cutoff) || (resolved && !(*branched) && *infeasible && !(*propagateagain) && !(*solvelpagain)));
      assert(*infeasible || (!resolved && !(*branched) && !(*cutoff) && !(*propagateagain) && !(*solvelpagain)));
      assert(!(*propagateagain) || (resolved && !(*branched) && !(*cutoff) && *infeasible));
      assert(!(*solvelpagain) || (resolved && !(*branched) && !(*cutoff) && *infeasible));
   }
   assert(!objinfeasible || *infeasible);
   assert(resolved == (*branched || *cutoff || *propagateagain || *solvelpagain));
   assert(*cutoff || *solvelpagain || SCIPsepastoreGetNCuts(sepastore) == 0);

   /* deactivate the cut forcing of the constraint enforcement */
   SCIPsepastoreEndForceCuts(sepastore);

   SCIPdebugMessage(" -> enforcing result: branched=%u, cutoff=%u, infeasible=%u, propagateagain=%u, solvelpagain=%u, resolved=%u\n",
      *branched, *cutoff, *infeasible, *propagateagain, *solvelpagain, resolved);

   return SCIP_OKAY;
}

/** applies the cuts stored in the separation store, or clears the store if the node can be cut off */
static
SCIP_RETCODE applyCuts(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_EVENTFILTER*     eventfilter,        /**< global event filter */
   SCIP_Bool             root,               /**< is this the initial root LP? */
   SCIP_Bool*            cutoff,             /**< pointer to whether the node can be cut off */
   SCIP_Bool*            propagateagain,     /**< pointer to store TRUE, if domain propagation should be applied again */
   SCIP_Bool*            solvelpagain        /**< pointer to store TRUE, if the node's LP has to be solved again */
   )
{
   assert(stat != NULL);
   assert(cutoff != NULL);
   assert(propagateagain != NULL);
   assert(solvelpagain != NULL);

   if( *cutoff )
   {
      /* the found cuts are of no use, because the node is infeasible anyway (or we have an error in the LP) */
      SCIP_CALL( SCIPsepastoreClearCuts(sepastore, blkmem, set, eventqueue, eventfilter, lp) );
   }
   else if( SCIPsepastoreGetNCuts(sepastore) > 0 )
   {
      SCIP_Longint olddomchgcount;

      olddomchgcount = stat->domchgcount;
      SCIP_CALL( SCIPsepastoreApplyCuts(sepastore, blkmem, set, stat, tree, lp, branchcand, eventqueue, eventfilter, root, cutoff) );
      *propagateagain = *propagateagain || (stat->domchgcount != olddomchgcount);
      *solvelpagain = TRUE;
   }

   return SCIP_OKAY;
}

/** updates the cutoff, propagateagain, and solverelaxagain status of the current solving loop */
static
void updateLoopStatus(
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   int                   depth,              /**< depth of current node */
   SCIP_Bool*            cutoff,             /**< pointer to store TRUE, if the node can be cut off */
   SCIP_Bool*            propagateagain,     /**< pointer to store TRUE, if domain propagation should be applied again */
   SCIP_Bool*            solverelaxagain     /**< pointer to store TRUE, if at least one relaxator should be called again */
   )
{
   SCIP_NODE* focusnode;
   int r;

   assert(set != NULL);
   assert(stat != NULL);
   assert(cutoff != NULL);
   assert(propagateagain != NULL);
   assert(solverelaxagain != NULL);

   /* check, if the path was cutoff */
   *cutoff = *cutoff || (tree->cutoffdepth <= depth);

   /* check if branching was already performed */
   if( tree->nchildren == 0 )
   {
      /* check, if the focus node should be repropagated */
      focusnode = SCIPtreeGetFocusNode(tree);
      *propagateagain = *propagateagain || SCIPnodeIsPropagatedAgain(focusnode);

      /* check, if one of the external relaxations should be solved again */
      for( r = 0; r < set->nrelaxs && !(*solverelaxagain); ++r )
         *solverelaxagain = !SCIPrelaxIsSolved(set->relaxs[r], stat);
   }
   else
   {
      /* if branching was performed, avoid another node loop iteration */
      *propagateagain = FALSE;
      *solverelaxagain = FALSE;
   }
}

/** solves the focus node */
static
SCIP_RETCODE solveNode(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_RELAXATION*      relaxation,         /**< global relaxation data */
   SCIP_PRICESTORE*      pricestore,         /**< pricing storage */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_CUTPOOL*         cutpool,            /**< global cut pool */
   SCIP_CONFLICT*        conflict,           /**< conflict analysis data */
   SCIP_EVENTFILTER*     eventfilter,        /**< event filter for global (not variable dependent) events */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_Bool*            cutoff,             /**< pointer to store whether the node can be cut off */
   SCIP_Bool*            unbounded,          /**< pointer to store whether the focus node is unbounded */
   SCIP_Bool*            infeasible,         /**< pointer to store whether the focus node's solution is infeasible */
   SCIP_Bool*            restart,            /**< should solving process be started again with presolving? */
   SCIP_Bool*            afternodeheur       /**< pointer to store whether AFTERNODE heuristics were already called */
   )
{
   SCIP_NODE* focusnode;
   SCIP_Longint lastdomchgcount;
   SCIP_Real restartfac;
   int lastlpcount;
   int actdepth;
   int nlperrors;
   int nloops;
   SCIP_Bool foundsol;
   SCIP_Bool focusnodehaslp;
   SCIP_Bool initiallpsolved;
   SCIP_Bool solverelaxagain;
   SCIP_Bool solvelpagain;
   SCIP_Bool propagateagain;
   SCIP_Bool fullpropagation;
   SCIP_Bool branched;
   SCIP_Bool forcedlpsolve;
   SCIP_Bool pricingaborted;

   assert(set != NULL);
   assert(stat != NULL);
   assert(prob != NULL);
   assert(tree != NULL);
   assert(primal != NULL);
   assert(SCIPsepastoreGetNCuts(sepastore) == 0);
   assert(SCIPconflictGetNConflicts(conflict) == 0);
   assert(cutoff != NULL);
   assert(unbounded != NULL);
   assert(infeasible != NULL);
   assert(restart != NULL);
   assert(afternodeheur != NULL);

   *cutoff = FALSE;
   *unbounded = FALSE;
   *infeasible = FALSE;
   *restart = FALSE;
   *afternodeheur = FALSE;
   pricingaborted = FALSE;

   focusnode = SCIPtreeGetFocusNode(tree);
   assert(focusnode != NULL);
   assert(SCIPnodeGetType(focusnode) == SCIP_NODETYPE_FOCUSNODE);
   actdepth = SCIPnodeGetDepth(focusnode);

   /** invalidate relaxation solution */
   SCIPrelaxationSetSolValid(relaxation, FALSE);

   /** clear the storage of external branching candidates */
   SCIPbranchcandClearExternCands(branchcand);

   SCIPdebugMessage("Processing node %"SCIP_LONGINT_FORMAT" in depth %d, %d siblings\n",
      stat->nnodes, actdepth, tree->nsiblings);
   SCIPdebugMessage("current pseudosolution: obj=%g\n", SCIPlpGetPseudoObjval(lp, set));
   /*debug(SCIPprobPrintPseudoSol(prob, set));*/

   /* check, if we want to solve the LP at the selected node:
    * - solve the LP, if the lp solve depth and frequency demand solving
    * - solve the root LP, if the LP solve frequency is set to 0
    * - solve the root LP, if there are continuous variables present
    * - don't solve the node if its cut off by the pseudo objective value anyway
    */
   focusnodehaslp = (set->lp_solvedepth == -1 || actdepth <= set->lp_solvedepth);
   focusnodehaslp = focusnodehaslp && (set->lp_solvefreq >= 1 && actdepth % set->lp_solvefreq == 0);
   focusnodehaslp = focusnodehaslp || (actdepth == 0 && set->lp_solvefreq == 0);
   focusnodehaslp = focusnodehaslp && SCIPsetIsLT(set, SCIPlpGetPseudoObjval(lp, set), primal->cutoffbound);
   SCIPtreeSetFocusNodeLP(tree, focusnodehaslp);

   /* call primal heuristics that should be applied before the node was solved */
   SCIP_CALL( SCIPprimalHeuristics(set, stat, primal, tree, lp, NULL, SCIP_HEURTIMING_BEFORENODE, &foundsol) );
   assert(SCIPbufferGetNUsed(set->buffer) == 0);

   /* if diving produced an LP error, switch back to non-LP node */
   if( lp->resolvelperror )
      SCIPtreeSetFocusNodeLP(tree, FALSE);

   /* external node solving loop:
    *  - propagate domains
    *  - solve SCIP_LP
    *  - enforce constraints
    * if a constraint handler adds constraints to enforce its own constraints, both, propagation and LP solving
    * is applied again (if applicable on current node); however, if the new constraints don't have the enforce flag set,
    * it is possible, that the current infeasible solution is not cut off; in this case, we have to declare the solution
    * infeasible and perform a branching
    */
   lastdomchgcount = stat->domchgcount;
   lastlpcount = stat->lpcount;
   initiallpsolved = FALSE;
   nlperrors = 0;
   stat->npricerounds = 0;
   stat->nseparounds = 0;
   solverelaxagain = TRUE;
   solvelpagain = TRUE;
   propagateagain = TRUE;
   fullpropagation = TRUE;
   forcedlpsolve = FALSE;
   nloops = 0;
   while( !(*cutoff) && (solverelaxagain || solvelpagain || propagateagain) && nlperrors < MAXNLPERRORS && !(*restart) )
   {
      SCIP_Bool lperror;
      SCIP_Bool solverelax;
      SCIP_Bool solvelp;
      SCIP_Bool propagate;
      SCIP_Bool forcedenforcement;

      assert(SCIPsepastoreGetNCuts(sepastore) == 0);

      nloops++;
      lperror = FALSE;
      solverelax = solverelaxagain;
      solverelaxagain = FALSE;
      solvelp = solvelpagain;
      solvelpagain = FALSE;
      propagate = propagateagain;
      propagateagain = FALSE;
      forcedenforcement = FALSE;

      /* update lower bound with the pseudo objective value, and cut off node by bounding */
      SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );

      /* domain propagation */
      if( propagate && !(*cutoff) )
      {
         SCIP_Bool lpwasflushed;
         SCIP_Longint oldnboundchgs;

         lpwasflushed = lp->flushed;
         oldnboundchgs = stat->nboundchgs;

         SCIP_CALL( propagateDomains(blkmem, set, stat, primal, tree, SCIPtreeGetCurrentDepth(tree), 0, fullpropagation, cutoff) );
         assert(SCIPbufferGetNUsed(set->buffer) == 0);

         fullpropagation = FALSE;

         /* check, if the path was cutoff */
         *cutoff = *cutoff || (tree->cutoffdepth <= actdepth);

         /* if the LP was flushed and is now no longer flushed, a bound change occurred, and the LP has to be resolved */
         solvelp = solvelp || (lpwasflushed && !lp->flushed);

         /* the number of bound changes was increased by the propagation call, thus the relaxation should be solved again */
         solverelax = solverelax || (stat->nboundchgs > oldnboundchgs);

         /* update lower bound with the pseudo objective value, and cut off node by bounding */
         SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );
      }
      assert(SCIPsepastoreGetNCuts(sepastore) == 0);

      /* call primal heuristics that are applicable after propagation loop */
      if( !(*cutoff) && !SCIPtreeProbing(tree) )
      {
         /* if the heuristics find a new incumbent solution, propagate again */
         SCIP_CALL( SCIPprimalHeuristics(set, stat, primal, tree, NULL, NULL, SCIP_HEURTIMING_AFTERPROPLOOP, &propagateagain) );
         assert(SCIPbufferGetNUsed(set->buffer) == 0);
      }
         
      /* solve external relaxations with non-negative priority */
      if( solverelax && !(*cutoff) )
      {
         /** clear the storage of external branching candidates */
         SCIPbranchcandClearExternCands(branchcand);

         SCIP_CALL( solveNodeRelax(set, stat, tree, actdepth, TRUE, cutoff, &propagateagain, &solvelpagain, &solverelaxagain) );
         assert(SCIPbufferGetNUsed(set->buffer) == 0);

         /* check, if the path was cutoff */
         *cutoff = *cutoff || (tree->cutoffdepth <= actdepth);

         /* apply found cuts */
         SCIP_CALL( applyCuts(blkmem, set, stat, tree, lp, sepastore, branchcand, eventqueue, eventfilter, (actdepth == 0),
               cutoff, &propagateagain, &solvelpagain) );

         /* update lower bound with the pseudo objective value, and cut off node by bounding */
         SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );
      }
      assert(SCIPsepastoreGetNCuts(sepastore) == 0);

      /* check, if we want to solve the LP at this node */
      if( solvelp && !(*cutoff) && SCIPtreeHasFocusNodeLP(tree) )
      {
         /* solve the node's LP */
         SCIP_CALL( solveNodeLP(blkmem, set, stat, prob, primal, tree, lp, pricestore, sepastore,
               cutpool, branchcand, conflict, eventfilter, eventqueue, initiallpsolved, cutoff, unbounded, 
               &lperror, &pricingaborted) );
         initiallpsolved = TRUE;
         SCIPdebugMessage(" -> LP status: %d, LP obj: %g, iter: %"SCIP_LONGINT_FORMAT", count: %d\n",
            SCIPlpGetSolstat(lp),
            *cutoff ? SCIPsetInfinity(set) : lperror ? -SCIPsetInfinity(set) : SCIPlpGetObjval(lp, set),
            stat->nlpiterations, stat->lpcount);

        /* check, if the path was cutoff */
         *cutoff = *cutoff || (tree->cutoffdepth <= actdepth);

         /* if an error occured during LP solving, switch to pseudo solution */
         if( lperror )
         {
            if( forcedlpsolve )
            {
               SCIPerrorMessage("(node %"SCIP_LONGINT_FORMAT") unresolved numerical troubles in LP %d cannot be dealt with\n",
                  stat->nnodes, stat->nlps);
               return SCIP_LPERROR;
            }
            SCIPtreeSetFocusNodeLP(tree, FALSE);
            nlperrors++;
            SCIPmessagePrintVerbInfo(set->disp_verblevel, SCIP_VERBLEVEL_FULL,
               "(node %"SCIP_LONGINT_FORMAT") unresolved numerical troubles in LP %d -- using pseudo solution instead (loop %d)\n",
               stat->nnodes, stat->nlps, nlperrors);
         }
         
         if( SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_TIMELIMIT || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_ITERLIMIT )
         {
            SCIPtreeSetFocusNodeLP(tree, FALSE);
            forcedenforcement = TRUE;
         }

         /* if we solve exactly, the LP claims to be infeasible but the infeasibility could not be proved,
          * we have to forget about the LP and use the pseudo solution instead
          */
         if( !(*cutoff) && !lperror && set->misc_exactsolve && SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_INFEASIBLE
            && SCIPnodeGetLowerbound(focusnode) < primal->cutoffbound )
         {
            if( SCIPbranchcandGetNPseudoCands(branchcand) == 0 && prob->ncontvars > 0 )
            {
               SCIPerrorMessage("(node %"SCIP_LONGINT_FORMAT") could not prove infeasibility of LP %d, all variables are fixed, %d continuous vars\n",
                  stat->nnodes, stat->nlps, prob->ncontvars);
               SCIPerrorMessage("(node %"SCIP_LONGINT_FORMAT")  -> have to call PerPlex() (feature not yet implemented)\n", stat->nnodes);
               /**@todo call PerPlex */
               return SCIP_LPERROR;
            }
            else
            {
               SCIPtreeSetFocusNodeLP(tree, FALSE);
               SCIPmessagePrintVerbInfo(set->disp_verblevel, SCIP_VERBLEVEL_FULL,
                  "(node %"SCIP_LONGINT_FORMAT") could not prove infeasibility of LP %d -- using pseudo solution (%d unfixed vars) instead\n",
                  stat->nnodes, stat->nlps, SCIPbranchcandGetNPseudoCands(branchcand));
            }
         }

         /* update lower bound with the pseudo objective value, and cut off node by bounding */
         SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );
      }
      assert(SCIPsepastoreGetNCuts(sepastore) == 0);
      assert(*cutoff || !SCIPtreeHasFocusNodeLP(tree) || (lp->flushed && lp->solved));

      /* solve external relaxations with negative priority */
      if( solverelax && !(*cutoff) )
      {
         SCIP_CALL( solveNodeRelax(set, stat, tree, actdepth, FALSE, cutoff, &propagateagain, &solvelpagain, &solverelaxagain) );
         assert(SCIPbufferGetNUsed(set->buffer) == 0);

         /* check, if the path was cutoff */
         *cutoff = *cutoff || (tree->cutoffdepth <= actdepth);

         /* apply found cuts */
         SCIP_CALL( applyCuts(blkmem, set, stat, tree, lp, sepastore, branchcand, eventqueue, eventfilter, (actdepth == 0),
               cutoff, &propagateagain, &solvelpagain) );
         
         /* update lower bound with the pseudo objective value, and cut off node by bounding */
         SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );
      }
      assert(SCIPsepastoreGetNCuts(sepastore) == 0);

      /* update the cutoff, propagateagain, and solverelaxagain status of current solving loop */
      updateLoopStatus(set, stat, tree, actdepth, cutoff, &propagateagain, &solverelaxagain);

      /* call primal heuristics that should be applied after the LP relaxation of the node was solved;
       * if this is the first loop of the first run's root node, call also AFTERNODE heuristics already here, since
       * they might help to improve the primal bound, thereby producing additional reduced cost strengthenings and
       * strong branching bound fixings
       */
      if( !(*cutoff) || SCIPtreeGetNNodes(tree) > 0 )
      {
         if( actdepth == 0 && stat->nruns == 1 && nloops == 1 )
         {
            SCIP_CALL( SCIPprimalHeuristics(set, stat, primal, tree, lp, NULL,
                  SCIP_HEURTIMING_AFTERLPLOOP | SCIP_HEURTIMING_AFTERNODE, &foundsol) );
            *afternodeheur = TRUE; /* the AFTERNODE heuristics should node be called again after the node */
         }
         else
         {
            SCIP_CALL( SCIPprimalHeuristics(set, stat, primal, tree, lp, NULL, SCIP_HEURTIMING_AFTERLPLOOP, &foundsol) );
         }
         assert(SCIPbufferGetNUsed(set->buffer) == 0);
            
         /* heuristics might have found a solution or set the cutoff bound such that the current node is cut off */
         SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );
      }

      /* check if heuristics leave us with an invalid LP */
      if( lp->resolvelperror )
      {
         if( forcedlpsolve )
         {
            SCIPerrorMessage("(node %"SCIP_LONGINT_FORMAT") unresolved numerical troubles in LP %d cannot be dealt with\n",
               stat->nnodes, stat->nlps);
            return SCIP_LPERROR;
         }
         SCIPtreeSetFocusNodeLP(tree, FALSE);
         nlperrors++;
         SCIPmessagePrintVerbInfo(set->disp_verblevel, SCIP_VERBLEVEL_FULL,
            "(node %"SCIP_LONGINT_FORMAT") unresolved numerical troubles in LP %d -- using pseudo solution instead (loop %d)\n",
            stat->nnodes, stat->nlps, nlperrors);
      }
    
      /* if an improved solution was found, propagate and solve the relaxations again */
      if( foundsol )
      {
         propagateagain = TRUE;
         solvelpagain = TRUE;
         solverelaxagain = TRUE;
         markRelaxsUnsolved(set, relaxation);
      }
    
      /* enforce constraints */
      branched = FALSE;
      if( !(*cutoff) && !solverelaxagain && !solvelpagain && !propagateagain )
      {
         /* if the solution changed since the last enforcement, we have to completely reenforce it; otherwise, we
          * only have to enforce the additional constraints added in the last enforcement, but keep the infeasible
          * flag TRUE in order to not declare the infeasible solution feasible due to disregarding the already
          * enforced constraints
          */
         if( lastdomchgcount != stat->domchgcount || lastlpcount != stat->lpcount )
         {
            lastdomchgcount = stat->domchgcount;
            lastlpcount = stat->lpcount;
            *infeasible = FALSE;
         }
        
         /* call constraint enforcement */
         SCIP_CALL( enforceConstraints(blkmem, set, stat, tree, lp, relaxation, sepastore, branchcand,
               &branched, cutoff, infeasible, &propagateagain, &solvelpagain, &solverelaxagain, forcedenforcement) );
         assert(branched == (tree->nchildren > 0));
         assert(!branched || (!(*cutoff) && *infeasible && !propagateagain && !solvelpagain));
         assert(!(*cutoff) || (!branched && *infeasible && !propagateagain && !solvelpagain));
         assert(*infeasible || (!branched && !(*cutoff) && !propagateagain && !solvelpagain));
         assert(!propagateagain || (!branched && !(*cutoff) && *infeasible));
         assert(!solvelpagain || (!branched && !(*cutoff) && *infeasible));

         assert(SCIPbufferGetNUsed(set->buffer) == 0);

         /* apply found cuts */
         SCIP_CALL( applyCuts(blkmem, set, stat, tree, lp, sepastore, branchcand, eventqueue, eventfilter, (actdepth == 0),
               cutoff, &propagateagain, &solvelpagain) );

         /* update lower bound with the pseudo objective value, and cut off node by bounding */
         SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );

         /* update the cutoff, propagateagain, and solverelaxagain status of current solving loop */
         updateLoopStatus(set, stat, tree, actdepth, cutoff, &propagateagain, &solverelaxagain);
      }
      assert(SCIPsepastoreGetNCuts(sepastore) == 0);

      /* The enforcement detected no infeasibility, so, no branching was performed,
       * but the pricing was aborted and the current feasible solution does not have to be the 
       * best solution in the current subtree --> we have to do a pseudo branching,
       * so we set infeasible TRUE and add the current solution to the solution pool
       */
      if ( pricingaborted && !(*infeasible) && !(*cutoff) )
      {
         SCIP_Bool stored;
         SCIP_SOL* sol;

         SCIP_CALL( SCIPsolCreateCurrentSol(&sol, blkmem, set, stat, primal, tree, lp, NULL) );
         SCIP_CALL( SCIPprimalTrySolFree(primal, blkmem, set, stat, prob, tree, lp, eventfilter, &sol, FALSE, TRUE, TRUE, TRUE, &stored) );

         *infeasible = TRUE;
      }

      /* if the node is infeasible, but no constraint handler could resolve the infeasibility
       * -> branch on LP or the pseudo solution
       * -> e.g. select non-fixed binary or integer variable x with value x', create three
       *    sons: x <= x'-1, x = x', and x >= x'+1.
       *    In the left and right branch, the current solution is cut off. In the middle
       *    branch, the constraints can hopefully reduce domains of other variables to cut
       *    off the current solution.
       * In LP branching, we cannot allow adding constraints, because this does not necessary change the LP and can
       * therefore lead to an infinite loop.
       */
      forcedlpsolve = FALSE;
      if( *infeasible && !(*cutoff) && !(*unbounded) && !solverelaxagain && !solvelpagain && !propagateagain && !branched )
      {
         SCIP_RESULT result;
         int nlpcands;

         result = SCIP_DIDNOTRUN;

         if( SCIPtreeHasFocusNodeLP(tree) )
         {
            SCIP_CALL( SCIPbranchcandGetLPCands(branchcand, set, stat, lp, NULL, NULL, NULL, &nlpcands, NULL) );
         }
         else
            nlpcands = 0;

         if( nlpcands > 0 )
         {
            /* branch on LP solution */
            SCIPdebugMessage("infeasibility in depth %d was not resolved: branch on LP solution with %d fractionals\n",
               SCIPnodeGetDepth(focusnode), nlpcands);
            SCIP_CALL( SCIPbranchExecLP(blkmem, set, stat, tree, lp, sepastore, branchcand, eventqueue,
                  primal->cutoffbound, FALSE, &result) );
            assert(SCIPbufferGetNUsed(set->buffer) == 0);
            assert(result != SCIP_DIDNOTRUN);
         }
         else 
         {
            if( SCIPbranchcandGetNExternCands(branchcand) > 0 )
            {
               /* branch on external candidates */
               SCIPdebugMessage("infeasibility in depth %d was not resolved: branch on %d external branching candidates.\n",
                  SCIPnodeGetDepth(focusnode), SCIPbranchcandGetNExternCands(branchcand));
               SCIP_CALL( SCIPbranchExecExtern(blkmem, set, stat, tree, lp, sepastore, branchcand, eventqueue,
                     primal->cutoffbound, TRUE, &result) );
               assert(SCIPbufferGetNUsed(set->buffer) == 0);
            }

            if( result == SCIP_DIDNOTRUN )
            {
               /* branch on pseudo solution */
               SCIPdebugMessage("infeasibility in depth %d was not resolved: branch on pseudo solution with %d unfixed integers\n",
                  SCIPnodeGetDepth(focusnode), SCIPbranchcandGetNPseudoCands(branchcand));
               SCIP_CALL( SCIPbranchExecPseudo(blkmem, set, stat, tree, lp, branchcand, eventqueue,
                     primal->cutoffbound, TRUE, &result) );
               assert(SCIPbufferGetNUsed(set->buffer) == 0);
            }
         }
         
         switch( result )
         {
         case SCIP_CUTOFF:
            assert(tree->nchildren == 0);
            *cutoff = TRUE;
            SCIPdebugMessage(" -> branching rule detected cutoff\n");
            break;
         case SCIP_CONSADDED:
            assert(tree->nchildren == 0);
            if( nlpcands > 0 )
            {
               SCIPerrorMessage("LP branching rule added constraint, which was not allowed this time\n");
               return SCIP_INVALIDRESULT;
            }
            propagateagain = TRUE;
            solvelpagain = TRUE;
            solverelaxagain = TRUE;
            markRelaxsUnsolved(set, relaxation);
            break;
         case SCIP_REDUCEDDOM:
            assert(tree->nchildren == 0);
            propagateagain = TRUE;
            solvelpagain = TRUE;
            solverelaxagain = TRUE;
            markRelaxsUnsolved(set, relaxation);
            break;
         case SCIP_SEPARATED:
            assert(tree->nchildren == 0);
            assert(SCIPsepastoreGetNCuts(sepastore) > 0);
            solvelpagain = TRUE;
            solverelaxagain = TRUE;
            markRelaxsUnsolved(set, relaxation);
            break;
         case SCIP_BRANCHED:
            assert(tree->nchildren >= 1);
            assert(SCIPsepastoreGetNCuts(sepastore) == 0);
            branched = TRUE;
            break;
         case SCIP_DIDNOTRUN:
            /* all integer variables in the infeasible solution are fixed,
             * - if no continuous variables exist and all variables are known, the infeasible pseudo solution is completely
             *   fixed, and the node can be cut off
             * - if at least one continuous variable exists or we do not know all variables due to external pricers, we
             *   cannot resolve the infeasibility by branching -> solve LP (and maybe price in additional variables)
             */
            assert(tree->nchildren == 0);
            assert(SCIPsepastoreGetNCuts(sepastore) == 0);
            assert(SCIPbranchcandGetNPseudoCands(branchcand) == 0);

            if( prob->ncontvars == 0 && set->nactivepricers == 0 )
            {
               *cutoff = TRUE;
               SCIPdebugMessage(" -> cutoff because all variables are fixed in current node\n");
            }
            else
            {
               assert(!SCIPtreeHasFocusNodeLP(tree) || pricingaborted); /* feasible LP solutions with all integers fixed must be feasible */

               if( SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_TIMELIMIT || SCIPlpGetSolstat(lp) == SCIP_LPSOLSTAT_ITERLIMIT || SCIPsolveIsStopped(set, stat, FALSE) )
               {
                  SCIP_NODE* node;
               
                  /* as we hit the time or iteration limit or another interrupt (e.g., gap limit), we do not want to solve the LP again.
                   * in order to terminate correctly, we create a "branching" with only one child node 
                   * that is a copy of the focusnode 
                   */
                  SCIP_CALL( SCIPnodeCreateChild(&node, blkmem, set, stat, tree, 1.0, focusnode->estimate) );
                  assert(tree->nchildren >= 1);
                  assert(SCIPsepastoreGetNCuts(sepastore) == 0);
                  branched = TRUE;
               }
               else
               {
                  if ( pricingaborted )
                  {
                     SCIPerrorMessage("pricing was aborted, but no branching could be created!\n", result);
                     return SCIP_INVALIDRESULT;
                  }

                  SCIPmessagePrintVerbInfo(set->disp_verblevel, SCIP_VERBLEVEL_HIGH,
                     "(node: %"SCIP_LONGINT_FORMAT") forcing the solution of an LP ...\n", stat->nnodes, stat->nlps);

                  /* solve the LP in the next loop */
                  SCIPtreeSetFocusNodeLP(tree, TRUE);
                  solvelpagain = TRUE;
                  forcedlpsolve = TRUE; /* this LP must be solved without error - otherwise we have to abort */
               }            
            }
            break;
         default:
            SCIPerrorMessage("invalid result code <%d> from SCIPbranchLP(), SCIPbranchExt() or SCIPbranchPseudo()\n", result);
            return SCIP_INVALIDRESULT;
         }  /*lint !e788*/
         assert(*cutoff || solvelpagain || propagateagain || branched); /* something must have been done */
         assert(!(*cutoff) || (!solvelpagain && !propagateagain && !branched));
         assert(!solvelpagain || (!(*cutoff) && !branched));
         assert(!propagateagain || (!(*cutoff) && !branched));
         assert(!branched || (!solvelpagain && !propagateagain));
         assert(branched == (tree->nchildren > 0));

         /* apply found cuts */
         SCIP_CALL( applyCuts(blkmem, set, stat, tree, lp, sepastore, branchcand, eventqueue, eventfilter, (actdepth == 0),
               cutoff, &propagateagain, &solvelpagain) );

         /* update lower bound with the pseudo objective value, and cut off node by bounding */
         SCIP_CALL( applyBounding(blkmem, set, stat, prob, primal, tree, lp, conflict, cutoff) );

         /* update the cutoff, propagateagain, and solverelaxagain status of current solving loop */
         updateLoopStatus(set, stat, tree, actdepth, cutoff, &propagateagain, &solverelaxagain);
      }

      /* check for immediate restart */
      *restart = *restart 
         || (actdepth == 0 && (set->presol_maxrestarts == -1 || stat->nruns <= set->presol_maxrestarts) && set->nactivepricers == 0
            && (stat->userrestart 
               || (stat->nrootintfixingsrun > set->presol_immrestartfac * (prob->nvars - prob->ncontvars)
                  && (stat->nruns == 1 || prob->nvars <= (1.0-set->presol_restartminred) * stat->prevrunnvars))) );

      SCIPdebugMessage("node solving iteration %d finished: cutoff=%u, propagateagain=%u, solverelaxagain=%u, solvelpagain=%u, nlperrors=%d, restart=%u\n",
         nloops, *cutoff, propagateagain, solverelaxagain, solvelpagain, nlperrors, *restart);
   }
   assert(SCIPsepastoreGetNCuts(sepastore) == 0);
   assert(*cutoff || SCIPconflictGetNConflicts(conflict) == 0);

   /* flush the conflict set storage */
   SCIP_CALL( SCIPconflictFlushConss(conflict, blkmem, set, stat, prob, tree) );

   /* check for too many LP errors */
   if( nlperrors >= MAXNLPERRORS )
   {
      SCIPerrorMessage("(node %"SCIP_LONGINT_FORMAT") unresolved numerical troubles in LP %d -- aborting\n", stat->nnodes, stat->nlps);
      return SCIP_LPERROR;
   }

   /* check for final restart */
   restartfac = set->presol_subrestartfac;
   if( actdepth == 0 )
      restartfac = MIN(restartfac, set->presol_restartfac);
   *restart = *restart
      || ((set->presol_maxrestarts == -1 || stat->nruns <= set->presol_maxrestarts) && set->nactivepricers == 0
         && (stat->userrestart || 
            (stat->nrootintfixingsrun > restartfac * (prob->nvars - prob->ncontvars)
               && (stat->nruns == 1 || prob->nvars <= (1.0-set->presol_restartminred) * stat->prevrunnvars))) );

   /* remember root LP solution */
   if( actdepth == 0 && !(*cutoff) && !(*unbounded) )
      SCIPprobStoreRootSol(prob, set, stat, lp, SCIPtreeHasFocusNodeLP(tree));

   /* check for cutoff */
   if( *cutoff )
   {
      SCIPdebugMessage("node is cut off\n");
      SCIPnodeUpdateLowerbound(focusnode, stat, SCIPsetInfinity(set));
      *infeasible = TRUE;
      *restart = FALSE;
   }
   
   return SCIP_OKAY;
}

/** if feasible, adds current solution to the solution storage */
static
SCIP_RETCODE addCurrentSolution(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_EVENTFILTER*     eventfilter         /**< event filter for global (not variable dependent) events */
   )
{
   SCIP_SOL* sol;
   SCIP_Bool foundsol;

   /* found a feasible solution */
   if( SCIPtreeHasFocusNodeLP(tree) )
   {
      /* start clock for LP solutions */
      SCIPclockStart(stat->lpsoltime, set);

      /* add solution to storage */
      SCIP_CALL( SCIPsolCreateLPSol(&sol, blkmem, set, stat, primal, tree, lp, NULL) );
      if( set->misc_exactsolve )
      {
         /* if we want to solve exactly, we have to check the solution exactly again */
         SCIP_CALL( SCIPprimalTrySolFree(primal, blkmem, set, stat, prob, tree, lp, eventfilter, &sol,
               FALSE, TRUE, TRUE, TRUE, &foundsol) );
      }
      else
      {
         SCIP_CALL( SCIPprimalAddSolFree(primal, blkmem, set, stat, prob, tree, lp, eventfilter, &sol, &foundsol) );
      }
      if( foundsol )
         stat->nlpsolsfound++;

      /* stop clock for LP solutions */
      SCIPclockStop(stat->lpsoltime, set);
   }
   else
   {
      /* start clock for pseudo solutions */
      SCIPclockStart(stat->pseudosoltime, set);

      /* add solution to storage */
      SCIP_CALL( SCIPsolCreatePseudoSol(&sol, blkmem, set, stat, primal, tree, lp, NULL) );
      if( set->misc_exactsolve )
      {
         /* if we want to solve exactly, we have to check the solution exactly again */
         SCIP_CALL( SCIPprimalTrySolFree(primal, blkmem, set, stat, prob, tree, lp, eventfilter, &sol,
               FALSE, TRUE, TRUE, TRUE, &foundsol) );
      }
      else
      {
         SCIP_CALL( SCIPprimalAddSolFree(primal, blkmem, set, stat, prob, tree, lp, eventfilter, &sol, &foundsol) );
      }

      /* stop clock for pseudo solutions */
      SCIPclockStop(stat->pseudosoltime, set);

      if( foundsol )
         stat->npssolsfound++;
   }

   return SCIP_OKAY;
}

/** main solving loop */
SCIP_RETCODE SCIPsolveCIP(
   BMS_BLKMEM*           blkmem,             /**< block memory buffers */
   SCIP_SET*             set,                /**< global SCIP settings */
   SCIP_STAT*            stat,               /**< dynamic problem statistics */
   SCIP_MEM*             mem,                /**< block memory pools */
   SCIP_PROB*            prob,               /**< transformed problem after presolve */
   SCIP_PRIMAL*          primal,             /**< primal data */
   SCIP_TREE*            tree,               /**< branch and bound tree */
   SCIP_LP*              lp,                 /**< LP data */
   SCIP_RELAXATION*      relaxation,         /**< global relaxation data */
   SCIP_PRICESTORE*      pricestore,         /**< pricing storage */
   SCIP_SEPASTORE*       sepastore,          /**< separation storage */
   SCIP_CUTPOOL*         cutpool,            /**< global cut pool */
   SCIP_BRANCHCAND*      branchcand,         /**< branching candidate storage */
   SCIP_CONFLICT*        conflict,           /**< conflict analysis data */
   SCIP_EVENTFILTER*     eventfilter,        /**< event filter for global (not variable dependent) events */
   SCIP_EVENTQUEUE*      eventqueue,         /**< event queue */
   SCIP_Bool*            restart             /**< should solving process be started again with presolving? */
   )
{
   SCIP_NODESEL* nodesel;
   SCIP_NODE* focusnode;
   SCIP_NODE* nextnode;
   SCIP_EVENT event;
   SCIP_Real restartfac;
   SCIP_Real restartconfnum;
   int nnodes;
   int depth;
   SCIP_Bool cutoff;
   SCIP_Bool unbounded;
   SCIP_Bool infeasible;
   SCIP_Bool foundsol;

   assert(set != NULL);
   assert(blkmem != NULL);
   assert(stat != NULL);
   assert(prob != NULL);
   assert(tree != NULL);
   assert(lp != NULL);
   assert(pricestore != NULL);
   assert(sepastore != NULL);
   assert(branchcand != NULL);
   assert(cutpool != NULL);
   assert(primal != NULL);
   assert(eventfilter != NULL);
   assert(eventqueue != NULL);
   assert(restart != NULL);

   /* check for immediate restart (if problem solving marked to be restarted was aborted) */
   restartfac = set->presol_subrestartfac;
   if( SCIPtreeGetCurrentDepth(tree) == 0 )
      restartfac = MIN(restartfac, set->presol_restartfac);
   *restart = (set->presol_maxrestarts == -1 || stat->nruns <= set->presol_maxrestarts) && set->nactivepricers == 0
      && (stat->userrestart
         || (stat->nrootintfixingsrun > restartfac * (prob->nvars - prob->ncontvars)
            && (stat->nruns == 1 || prob->nvars <= (1.0-set->presol_restartminred) * stat->prevrunnvars)) );

   /* calculate the number of successful conflict analysis calls that should trigger a restart */
   if( set->conf_restartnum > 0 )
   {
      int i;

      restartconfnum = (SCIP_Real)set->conf_restartnum;
      for( i = 0; i < stat->nconfrestarts; ++i )
         restartconfnum *= set->conf_restartfac;
   }
   else
      restartconfnum = SCIP_REAL_MAX;
   assert(restartconfnum >= 0.0);

   /* switch status to UNKNOWN */
   stat->status = SCIP_STATUS_UNKNOWN;

   nextnode = NULL;
   unbounded = FALSE;

   while( !SCIPsolveIsStopped(set, stat, TRUE) && !(*restart) )
   {
      SCIP_Longint nsuccessconflicts;
      SCIP_Bool afternodeheur;

      assert(SCIPbufferGetNUsed(set->buffer) == 0);

      foundsol = FALSE;
      infeasible = FALSE;

      do
      {
         /* update the memory saving flag, switch algorithms respectively */
         SCIPstatUpdateMemsaveMode(stat, set, mem);

         /* get the current node selector */
         nodesel = SCIPsetGetNodesel(set, stat);

         /* inform tree about the current node selector */
         SCIP_CALL( SCIPtreeSetNodesel(tree, set, stat, nodesel) );

         /* the next node was usually already selected in the previous solving loop before the primal heuristics were
          * called, because they need to know, if the next node will be a child/sibling (plunging) or not;
          * if the heuristics found a new best solution that cut off some of the nodes, the node selector must be called
          * again, because the selected next node may be invalid due to cut off
          */
         if( nextnode == NULL )
         {
            /* select next node to process */
            SCIP_CALL( SCIPnodeselSelect(nodesel, set, &nextnode) );
         }
         focusnode = nextnode;
         nextnode = NULL;
         assert(SCIPbufferGetNUsed(set->buffer) == 0);

         /* start node activation timer */
         SCIPclockStart(stat->nodeactivationtime, set);

         /* focus selected node */
         SCIP_CALL( SCIPnodeFocus(&focusnode, blkmem, set, stat, prob, primal, tree, lp, branchcand, conflict,
               eventfilter, eventqueue, &cutoff) );
         if( cutoff )
            stat->ndelayedcutoffs++;

         /* stop node activation timer */
         SCIPclockStop(stat->nodeactivationtime, set);

         assert(SCIPbufferGetNUsed(set->buffer) == 0);
      }
      while( cutoff ); /* select new node, if the current one was located in a cut off subtree */

      assert(SCIPtreeGetCurrentNode(tree) == focusnode);
      assert(SCIPtreeGetFocusNode(tree) == focusnode);

      /* if no more node was selected, we finished optimization */
      if( focusnode == NULL )
      {
         assert(SCIPtreeGetNNodes(tree) == 0);
         break;
      }

      /* update maxdepth and node count statistics */
      depth = SCIPnodeGetDepth(focusnode);
      stat->maxdepth = MAX(stat->maxdepth, depth);
      stat->maxtotaldepth = MAX(stat->maxtotaldepth, depth);
      stat->nnodes++;
      stat->ntotalnodes++;

      /* issue NODEFOCUSED event */
      SCIP_CALL( SCIPeventChgType(&event, SCIP_EVENTTYPE_NODEFOCUSED) );
      SCIP_CALL( SCIPeventChgNode(&event, focusnode) );
      SCIP_CALL( SCIPeventProcess(&event, set, NULL, NULL, NULL, eventfilter) );

      /* solve focus node */
      SCIP_CALL( solveNode(blkmem, set, stat, prob, primal, tree, lp, relaxation, pricestore, sepastore, branchcand, 
            cutpool, conflict, eventfilter, eventqueue, &cutoff, &unbounded, &infeasible, restart, &afternodeheur) );
      assert(!cutoff || infeasible);
      assert(SCIPbufferGetNUsed(set->buffer) == 0);
      assert(SCIPtreeGetCurrentNode(tree) == focusnode);
      assert(SCIPtreeGetFocusNode(tree) == focusnode);

      /* check for restart */
      if( !(*restart) )
      {
         /* change color of node in VBC output */
         SCIPvbcSolvedNode(stat->vbc, stat, focusnode);

         /* check, if the current solution is feasible */
         if( !infeasible )
         {
            assert(!SCIPtreeHasFocusNodeLP(tree) || (lp->flushed && lp->solved));
            assert(!cutoff);

            /* node solution is feasible: add it to the solution store */
            SCIP_CALL( addCurrentSolution(blkmem, set, stat, prob, primal, tree, lp, eventfilter) );

            /* issue NODEFEASIBLE event */
            SCIP_CALL( SCIPeventChgType(&event, SCIP_EVENTTYPE_NODEFEASIBLE) );
            SCIP_CALL( SCIPeventChgNode(&event, focusnode) );
            SCIP_CALL( SCIPeventProcess(&event, set, NULL, NULL, NULL, eventfilter) );
         }
         else if( !unbounded )
         {
            /* node solution is not feasible */
            if( tree->nchildren == 0 )
            {
               /* issue NODEINFEASIBLE event */
               SCIP_CALL( SCIPeventChgType(&event, SCIP_EVENTTYPE_NODEINFEASIBLE) );

               /* increase the cutoff counter of the branching variable */
               if( stat->lastbranchvar != NULL )
               {
                  SCIP_CALL( SCIPvarIncCutoffSum(stat->lastbranchvar, stat, stat->lastbranchdir, 1.0) );
               }
               /**@todo if last branching variable is unknown, retrieve it from the nodes' boundchg arrays */
            }
            else
            {
               /* issue NODEBRANCHED event */
               SCIP_CALL( SCIPeventChgType(&event, SCIP_EVENTTYPE_NODEBRANCHED) );
            }
            SCIP_CALL( SCIPeventChgNode(&event, focusnode) );
            SCIP_CALL( SCIPeventProcess(&event, set, NULL, NULL, NULL, eventfilter) );
         }
         assert(SCIPbufferGetNUsed(set->buffer) == 0);

         /* if no branching was created, the node was not cut off, but it's lower bound is still smaller than
          * the cutoff bound, we have to branch on a non-fixed variable;
          * this can happen, if we want to solve exactly, the current solution was declared feasible by the
          * constraint enforcement, but in exact solution checking it was found out to be infeasible;
          * in this case, no branching would have been generated by the enforcement of constraints, but we
          * have to further investigate the current sub tree
          */
         if( !cutoff && !unbounded && tree->nchildren == 0 && SCIPnodeGetLowerbound(focusnode) < primal->cutoffbound )
         {
            SCIP_RESULT result;

            assert(set->misc_exactsolve);

            do
            {
               result = SCIP_DIDNOTRUN;
               if( SCIPbranchcandGetNPseudoCands(branchcand) == 0 )
               {
                  if( prob->ncontvars > 0 )
                  {
                     /**@todo call PerPlex */
                     SCIPerrorMessage("cannot branch on all-fixed LP -- have to call PerPlex instead\n");
                  }
               }
               else
               {
                  SCIP_CALL( SCIPbranchExecPseudo(blkmem, set, stat, tree, lp, branchcand, eventqueue,
                        primal->cutoffbound, FALSE, &result) );
                  assert(result != SCIP_DIDNOTRUN);
               }
            }
            while( result == SCIP_REDUCEDDOM );
         }
         assert(SCIPbufferGetNUsed(set->buffer) == 0);

         /* select node to process in next solving loop; the primal heuristics need to know whether a child/sibling
          * (plunging) will be selected as next node or not
          */
         SCIP_CALL( SCIPnodeselSelect(nodesel, set, &nextnode) );
         assert(SCIPbufferGetNUsed(set->buffer) == 0);

         /* call primal heuristics that should be applied after the node was solved */
         nnodes = SCIPtreeGetNNodes(tree);
         if( !afternodeheur && (!cutoff || nnodes > 0) )
         {
            SCIP_CALL( SCIPprimalHeuristics(set, stat, primal, tree, lp, nextnode, SCIP_HEURTIMING_AFTERNODE, &foundsol) );
            assert(SCIPbufferGetNUsed(set->buffer) == 0);
         }

         /* if the heuristics found a new best solution that cut off some of the nodes, the node selector must be called
          * again, because the selected next node may be invalid due to cut off
          */
         assert(!tree->cutoffdelayed);
         if( nnodes != SCIPtreeGetNNodes(tree) || SCIPsolveIsStopped(set, stat, TRUE) )
            nextnode = NULL;
      }
      else if( !infeasible )
      {
         SCIP_SOL* sol;
         SCIP_Bool stored;

         SCIP_CALL( SCIPsolCreateCurrentSol(&sol, blkmem, set, stat, primal, tree, lp, NULL) );
         SCIP_CALL( SCIPprimalTrySolFree(primal, blkmem, set, stat, prob, tree, lp, eventfilter, &sol, FALSE, TRUE, TRUE, TRUE, &stored) );
      }
         
      /* trigger restart due to conflicts */
      nsuccessconflicts = SCIPconflictGetNPropSuccess(conflict) + SCIPconflictGetNInfeasibleLPSuccess(conflict)
         + SCIPconflictGetNBoundexceedingLPSuccess(conflict) + SCIPconflictGetNStrongbranchSuccess(conflict)
         + SCIPconflictGetNPseudoSuccess(conflict);
      if( nsuccessconflicts >= restartconfnum && set->nactivepricers == 0 )
      {
         SCIPmessagePrintVerbInfo(set->disp_verblevel, SCIP_VERBLEVEL_HIGH,
            "(run %d, node %"SCIP_LONGINT_FORMAT") restarting after %"SCIP_LONGINT_FORMAT" successful conflict analysis calls\n",
            stat->nruns, stat->nnodes, nsuccessconflicts);
         *restart = TRUE;
         stat->nconfrestarts++;
      }

      /* display node information line */
      SCIP_CALL( SCIPdispPrintLine(set, stat, NULL, (SCIPnodeGetDepth(focusnode) == 0) && infeasible && !foundsol) );

      SCIPdebugMessage("Processing of node %"SCIP_LONGINT_FORMAT" in depth %d finished. %d siblings, %d children, %d leaves left\n",
         stat->nnodes, SCIPnodeGetDepth(focusnode), tree->nsiblings, tree->nchildren, SCIPtreeGetNLeaves(tree));
      SCIPdebugMessage("**********************************************************************\n");
   }
   assert(SCIPbufferGetNUsed(set->buffer) == 0);

   SCIPdebugMessage("Problem solving finished (restart=%u)\n", *restart);

   /* if the current node is the only remaining node, and if its lower bound exceeds the upper bound, we have
    * to delete it manually in order to get to the SOLVED stage instead of thinking, that only the gap limit
    * was reached (this may happen, if the current node is the one defining the global lower bound and a
    * feasible solution with the same value was found at this node)
    */
   if( tree->focusnode != NULL && SCIPtreeGetNNodes(tree) == 0
      && SCIPsetIsGE(set, tree->focusnode->lowerbound, primal->cutoffbound) )
   {
      focusnode = NULL;
      SCIP_CALL( SCIPnodeFocus(&focusnode, blkmem, set, stat, prob, primal, tree, lp, branchcand, conflict,
            eventfilter, eventqueue, &cutoff) );
   }

   /* check whether we finished solving */
   if( SCIPtreeGetNNodes(tree) == 0 && SCIPtreeGetCurrentNode(tree) == NULL )
   {
      /* no restart necessary */
      *restart = FALSE;

      /* set the solution status */
      if( unbounded )
      {
         if( primal->nsols > 0 )
         {
            /* switch status to UNBOUNDED */
            stat->status = SCIP_STATUS_UNBOUNDED;
         }
         else
         {
            /* switch status to INFORUNB */
            stat->status = SCIP_STATUS_INFORUNBD;
         }
      }
      else if( primal->nsols == 0
         || SCIPsetIsGE(set, SCIPsolGetObj(primal->sols[0], set, prob), 
            SCIPprobInternObjval(prob, set, SCIPprobGetObjlim(prob, set))) )
      {
         /* switch status to INFEASIBLE */
         stat->status = SCIP_STATUS_INFEASIBLE;
      }
      else
      {
         /* switch status to OPTIMAL */
         stat->status = SCIP_STATUS_OPTIMAL;
      }
   }

   return SCIP_OKAY;
}
