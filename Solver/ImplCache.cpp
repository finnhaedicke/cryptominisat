/*
 * CryptoMiniSat
 *
 * Copyright (c) 2009-2011, Mate Soos and collaborators. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301  USA
*/

#include "ImplCache.h"

#ifndef __TRANSCACHE_H__
#define __TRANSCACHE_H__

#include "ThreadControl.h"
#include "CommandControl.h"
#include "VarReplacer.h"
#include "VarUpdateHelper.h"
#include "time_mem.h"
using std::cout;
using std::endl;

void ImplCache::clean(ThreadControl* control)
{
    assert(control->ok);
    assert(control->decisionLevel() == 0);

    double myTime = cpuTime();
    uint64_t numUpdated = 0;
    uint64_t numCleaned = 0;
    uint64_t numFreed = 0;

    //Free memory if possible
    for (Var var = 0; var < control->nVars(); var++) {
        //If replaced, merge it into the one that replaced it
        if (control->varData[var].elimed == ELIMED_VARREPLACER) {
            for(int i = 0; i < 2; i++) {
                const Lit litOrig = Lit(var, i);
                if (implCache[litOrig.toInt()].lits.empty())
                    continue;

                const Lit lit = control->varReplacer->getLitReplacedWith(litOrig);
                implCache[lit.toInt()].merge(
                    implCache[litOrig.toInt()].lits
                    , lit_Undef //nothing to add
                    , false //replaced, so 'non-learnt'
                    , lit //exclude this var
                    , control->seen
                );
            }
        }

        //Free it
        if (control->value(var) != l_Undef
            || control->varData[var].elimed == ELIMED_VARELIM
            || control->varData[var].elimed == ELIMED_VARREPLACER
        ) {
            vector<LitExtra> tmp1;
            numFreed += implCache[Lit(var, false).toInt()].lits.capacity();
            implCache[Lit(var, false).toInt()].lits.swap(tmp1);

            vector<LitExtra> tmp2;
            numFreed += implCache[Lit(var, true).toInt()].lits.capacity();
            implCache[Lit(var, true).toInt()].lits.swap(tmp2);
        }
    }

    vector<uint16_t>& inside = control->seen;
    vector<uint16_t>& nonLearnt = control->seen2;
    size_t wsLit = 0;
    for(vector<TransCache>::iterator
        trans = implCache.begin(), transEnd = implCache.end()
        ; trans != transEnd
        ; trans++, wsLit++
    ) {
        //Stats
        size_t origSize = trans->lits.size();
        size_t newSize = 0;

        //Update to replaced vars, remove vars already set or eliminated
        Lit vertLit = ~Lit::toLit(wsLit);
        vector<LitExtra>::iterator it = trans->lits.begin();
        vector<LitExtra>::iterator it2 = it;
        for (vector<LitExtra>::iterator end = trans->lits.end(); it != end; it++) {
            Lit lit = it->getLit();
            assert(lit.var() != vertLit.var());

            //Remove if value is set
            if (control->value(lit.var()) != l_Undef)
                continue;

            //Update to its replaced version
            if (control->varData[lit.var()].elimed == ELIMED_VARREPLACER
                || control->varData[lit.var()].elimed == ELIMED_QUEUED_VARREPLACER
            ) {
                lit = control->varReplacer->getLitReplacedWith(lit);

                //This would be tautological (and incorrect), so skip
                if (lit.var() == vertLit.var())
                    continue;
                numUpdated++;
            }

            //If updated version is eliminated, skip
            if (control->varData[lit.var()].elimed != ELIMED_NONE)
                continue;

            //If we have already visited this var, just skip over, but update nonLearnt
            if (inside[lit.toInt()]) {
                nonLearnt[lit.toInt()] |= (char)it2->getOnlyNLBin();
                continue;
            }

            inside[lit.toInt()] = true;
            nonLearnt[lit.toInt()] |= (char)it->getOnlyNLBin();
            *it2++ = LitExtra(lit, it->getOnlyNLBin());
            newSize++;
        }
        trans->lits.resize(newSize);

        //Now that we have gone through the list, go through once more to:
        //1) set nonLearnt right (above we might have it set later)
        //2) clear 'inside'
        //3) clear 'nonLearnt'
        for (vector<LitExtra>::iterator it = trans->lits.begin(), end = trans->lits.end(); it != end; it++) {
            Lit lit = it->getLit();

            //Clear 'inside'
            inside[lit.toInt()] = false;

            //Clear 'nonLearnt'
            const bool nLearnt = nonLearnt[lit.toInt()];
            nonLearnt[lit.toInt()] = false;

            //Set non-leartness correctly
            LitExtra(lit, nLearnt);
        }
        numCleaned += origSize-trans->lits.size();
    }

    if (control->conf.verbosity >= 1) {
        cout << "c Cache cleaned."
        << " Updated: " << std::setw(7) << numUpdated/1000 << " K"
        << " Cleaned: " << std::setw(7) << numCleaned/1000 << " K"
        << " Freed: " << std::setw(7) << numFreed/1000 << " K"
        << " T: " << std::setprecision(2) << std::fixed  << (cpuTime()-myTime) << endl;
    }
}

void ImplCache::handleNewData(
    vector<uint16_t>& val
    , Var var
    , Lit lit
    , uint32_t& bProp
    , uint32_t& bXProp
) {
    //Unfortunately, we cannot add the clauses, because that could mess up
    //the watchlists, which are being traversed by the callers, so we add these
    //new truths as delayed clauses, and add them at the end

    vector<Lit> tmp;

    //a->b and (-a)->b, so 'b'
    if  (val[lit.var()] == lit.sign()) {
        delayedClausesToAddNorm.push_back(lit);
        bProp++;
    } else {
        //a->b, and (-a)->(-b), so equivalent literal
        tmp.push_back(Lit(var, false));
        tmp.push_back(Lit(lit.var(), false));
        bool sign = lit.sign();
        delayedClausesToAddXor.push_back(std::make_pair(tmp, sign));
        bXProp++;
    }
}

bool ImplCache::addDelayedClauses(ThreadControl* control)
{
    assert(control->ok);

    //Add all delayed clauses
    for(vector<std::pair<vector<Lit>, bool> > ::const_iterator
        it = delayedClausesToAddXor.begin(), end = delayedClausesToAddXor.end()
        ; it != end
        ; it++
    ) {
        bool OK = true;
        for(vector<Lit>::const_iterator
            it2 = it->first.begin(), end2 = it->first.end()
            ; it2 != end2
            ; it2++
        ) {
            if (control->varData[it2->var()].elimed != ELIMED_NONE
                && control->varData[it2->var()].elimed != ELIMED_QUEUED_VARREPLACER
            ) {
                //Var has been eliminated one way or another. Don't add this clause
                OK = false;
                break;
            }
        }

        //If any of the variables have been eliminated (possible, if cache is used)
        //then don't add this clause
        if (!OK)
            continue;

        //Add the clause
        control->addXorClauseInt(it->first, it->second);

        //Check if this caused UNSAT
        if  (!control->ok)
            return false;
    }

    for(vector<Lit>::const_iterator
        it = delayedClausesToAddNorm.begin(), end = delayedClausesToAddNorm.end()
        ; it != end
        ; it++
    ) {
        //Build unit clause
        vector<Lit> tmp(1);
        tmp[0] = *it;

        //Add unit clause
        control->addClauseInt(tmp);

        //Check if this caused UNSAT
        if  (!control->ok)
            return false;
    }

    //Clear all
    delayedClausesToAddXor.clear();
    delayedClausesToAddNorm.clear();

    return true;
}

bool ImplCache::tryBoth(ThreadControl* control)
{
    assert(control->ok);
    assert(control->decisionLevel() == 0);
    bProp = 0;
    bXProp = 0;

    //Measuring time & usefulness
    double myTime = cpuTime();
    uint32_t backupTrailSize = control->trail.size();

    for (Var var = 0; var < control->nVars(); var++) {

        //If value is set or eliminated, skip
        if (control->value(var) != l_Undef
            || (control->varData[var].elimed != ELIMED_NONE
                && control->varData[var].elimed != ELIMED_QUEUED_VARREPLACER)
           )
            continue;

        //Try to do it
        tryVar(control, var);

        //Add all clauses that have been delayed
        if (!addDelayedClauses(control))
            goto end;
    }

    end:
    if (control->conf.verbosity >= 1) {
        cout << "c Cache " <<
        " BProp: " << bProp <<
        " Set: " << (control->trail.size() - backupTrailSize) <<
        " BXProp: " << bXProp <<
        " T: " << (cpuTime() - myTime) <<
        endl;
    }

    return control->ok;
}

void ImplCache::tryVar(
    ThreadControl* control
    , Var var
) {
    //Sanity check
    assert(control->ok);
    assert(control->decisionLevel() == 0);

    //Convenience
    vector<uint16_t>& seen = control->seen;
    vector<uint16_t>& val = control->seen2;

    Lit lit = Lit(var, false);

    const vector<LitExtra>& cache1 = implCache[lit.toInt()].lits;
    assert(control->watches.size() > ((~lit).toInt()));
    const vec<Watched>& ws1 = control->watches[(~lit).toInt()];
    const vector<LitExtra>& cache2 = implCache[(~lit).toInt()].lits;
    const vec<Watched>& ws2 = control->watches[(lit).toInt()];

    //Fill 'seen' and 'val' from cache
    for (vector<LitExtra>::const_iterator
        it = cache1.begin(), end = cache1.end()
        ; it != end
        ; it++
    ) {
        const Var var2 = it->getLit().var();
        if (control->varData[var2].elimed != ELIMED_NONE
            && control->varData[var2].elimed != ELIMED_QUEUED_VARREPLACER
        ) continue;

        seen[it->getLit().var()] = 1;
        val[it->getLit().var()] = it->getLit().sign();
    }

    //Fill 'seen' and 'val' from watch
    for (vec<Watched>::const_iterator
        it = ws1.begin(), end = ws1.end()
        ; it != end
        ; it++
    ) {
        if (!it->isBinary())
            continue;

        const Lit otherLit = it->getOtherLit();

        if (!seen[otherLit.var()]) {
            seen[otherLit.var()] = 1;
            val[otherLit.var()] = otherLit.sign();
        } else if (val[otherLit.var()] != otherLit.sign()) {
            //(a->b, a->-b) -> so 'a'
            delayedClausesToAddNorm.push_back(lit);
        }
    }
    //Okay, filled

    //Try to see if we propagate the same or opposite from the other end
    //Using cache
    for (vector<LitExtra>::const_iterator
        it = cache2.begin(), end = cache2.end()
        ; it != end
        ; it++
    ) {
        assert(it->getLit().var() != var);
        const Var var2 = it->getLit().var();

        //Only if the other one also contained it
        if (!seen[var2])
            continue;

        //If var has been elimed, skip
        if (control->varData[var2].elimed != ELIMED_NONE
            && control->varData[var2].elimed != ELIMED_QUEUED_VARREPLACER
        ) continue;

        handleNewData(val, var, it->getLit(), bProp, bXProp);
    }

    //Try to see if we propagate the same or opposite from the other end
    //Using binary clauses
    for (vec<Watched>::const_iterator it = ws2.begin(), end = ws2.end(); it != end; it++) {
        if (!it->isBinary())
            continue;

        assert(it->getOtherLit().var() != var);
        const Var var2 = it->getOtherLit().var();
        assert(var2 < control->nVars());

        //Only if the other one also contained it
        if (!seen[var2])
            continue;

        handleNewData(val, var, it->getOtherLit(), bProp, bXProp);
    }

    //Clear 'seen' and 'val'
    for (vector<LitExtra>::const_iterator it = cache1.begin(), end = cache1.end(); it != end; it++) {
        seen[it->getLit().var()] = false;
        val[it->getLit().var()] = false;
    }

    for (vec<Watched>::const_iterator it = ws1.begin(), end = ws1.end(); it != end; it++) {
        if (!it->isBinary())
            continue;

        seen[it->getOtherLit().var()] = false;
        val[it->getOtherLit().var()] = false;
    }
}

void TransCache::merge(
    vector<LitExtra>& otherLits
    , const Lit extraLit
    , const bool learnt
    , const Lit leaveOut
    , vector<uint16_t>& seen
) {
    //Mark every literal that is to be added in 'seen'
    for (size_t i = 0, size = otherLits.size(); i < size; i++) {
        const Lit lit = otherLits[i].getLit();
        const bool onlyNonLearnt = otherLits[i].getOnlyNLBin();

        seen[lit.toInt()] = 1 + (int)onlyNonLearnt;
    }
    //Handle extra lit
    if (extraLit != lit_Undef)
        seen[extraLit.toInt()] = 1 + (int)!learnt;

    //Everything that's already in the cache, set seen[] to zero
    //Also, if seen[] is 2, but it's marked learnt in the cache
    //mark it as non-learnt
    for (size_t i = 0, size = lits.size(); i < size; i++) {
        if (!learnt
            && !lits[i].getOnlyNLBin()
            && seen[lits[i].getLit().toInt()] == 2
        ) {
            lits[i].setOnlyNLBin();
        }

        seen[lits[i].getLit().toInt()] = 0;
    }

    //Whatever rests needs to be added
    for (size_t i = 0 ,size = otherLits.size(); i < size; i++) {
        const Lit lit = otherLits[i].getLit();
        if (seen[lit.toInt()]) {
            if (lit.var() != leaveOut.var())
                lits.push_back(LitExtra(lit, !learnt && otherLits[i].getOnlyNLBin()));
            seen[lit.toInt()] = 0;
        }
    }

    //Handle extra lit
    if (extraLit != lit_Undef && seen[extraLit.toInt()]) {
        if (extraLit.var() != leaveOut.var())
            lits.push_back(LitExtra(extraLit, !learnt));
        seen[extraLit.toInt()] = 0;
    }
}

void TransCache::updateVars(const std::vector< uint32_t >& outerToInter)
{
    for(size_t i = 0; i < lits.size(); i++) {
        lits[i] = LitExtra(getUpdatedLit(lits[i].getLit(), outerToInter), lits[i].getOnlyNLBin());
    }

}

void ImplCache::updateVars(
    vector<uint16_t>& seen
    , const std::vector< uint32_t >& outerToInter
    , const std::vector< uint32_t >& interToOuter2
) {
    updateBySwap(implCache, seen, interToOuter2);
    for(size_t i = 0; i < implCache.size(); i++) {
        implCache[i].updateVars(outerToInter);
    }
}


#endif //__TRANSCACHE_H__