/*! @file InteractionModWork.H
 * \brief Contains the class describing agent interactions at work
 */

#ifndef _INTERACTION_MOD_WORK_H_
#define _INTERACTION_MOD_WORK_H_

#include "InteractionModel.H"
#include "AgentDefinitions.H"

using namespace amrex;

/*! \brief One-on-one interaction between an infectious agent and a susceptible agent.
 *
 * This function defines the one-on-one interaction between an infectious agent and a
 * susceptible agent at work. */
template <typename PTDType>
AMREX_GPU_DEVICE AMREX_FORCE_INLINE
static void binaryInteractionWork ( const int a_i, /*!< Index of infectious agent */
                                    const int a_j, /*!< Index of susceptible agent */
                                    const PTDType& a_ptd, /*!< Particle tile data */
                                    const DiseaseParm* const a_lparm, /*!< disease paramters */
                                    const Real a_work_scale, /*!< Work scale */
                                    ParticleReal* const a_prob_ptr /*!< infection probability */)
{
    Real infect = a_lparm->infect;
    infect *= a_lparm->vac_eff;

    auto work_i_ptr = a_ptd.m_idata[IntIdx::work_i];
    auto workgroup_ptr = a_ptd.m_idata[IntIdx::workgroup];
    auto withdrawn_ptr = a_ptd.m_idata[IntIdx::withdrawn];

    if (withdrawn_ptr[a_i] || withdrawn_ptr[a_j]) { return; }

    //infect *= i_mask;
    //infect *= j_mask;
    ParticleReal prob = 1.0_prt;

    /* Workgroup transmission */
    if (workgroup_ptr[a_i] && (work_i_ptr[a_i] >= 0)) { // transmitter i at work
        if ((work_i_ptr[a_j] >= 0) && (workgroup_ptr[a_i] == workgroup_ptr[a_j])) {  // coworker
            prob *= 1.0_prt - infect * a_lparm->xmit_work * a_work_scale;
        }
    }

    Gpu::Atomic::Multiply(&a_prob_ptr[a_j], prob);
}

/*! \brief Class describing agent interactions at work */
template <typename AC, typename ACT, typename ACTD, typename A>
class InteractionModWork : public InteractionModel<AC,ACT,ACTD,A>
{
    public:

        /*! \brief null constructor */
        InteractionModWork() { }

        /*! \brief default destructor */
        virtual ~InteractionModWork() = default;

        /*! \brief Simulate agent interaction at work */
        virtual void interactAgents( AC&, MultiFab& );

    protected:

    private:
};

/*! Simulate the interactions between agents at workplace and compute
    the infection probability for each agent:

    + Create bins of agents if not already created (see
      #amrex::GetParticleBin, #amrex::DenseBins):
      + The bin size is 1 cell
      + #amrex::GetParticleBin maps a particle to its bin index
      + amrex::DenseBins::build() creates the bin-sorted array of particle indices and
        the offset array for each bin (where the offset of a bin is its starting location
        in the bin-sorted array of particle indices).

    + For each agent *i* in the bin-sorted array of agents:
      + Find its bin and the range of indices in the bin-sorted array for agents in its bin
      + If the agent is #Status::immune, do nothing.
      + If the agent is #Status::infected with the number of days infected (RealIdxDisease::disease_counter)
        less than the incubation length, do nothing.
      + Else, for each agent *j* in the same bin:
        + If the agent is #Status::immune, do nothing.
        + If the agent is #Status::infected with the number of days infected (RealIdxDisease::disease_counter)
          less than the incubation length, do nothing.
        + Else if *i* is not infected and *j* is infected, compute probability of *i* getting infected
          from *j* (see below).

    Summary of how the probability of agent A getting infected from agent B is computed:
    + Compute infection probability reduction factor from vaccine efficacy (#DiseaseParm::vac_eff)
    + At workplace - if agents are at work, and B has a workgroup and work location assigned: If A
      and B have the same workgroup and work location, use the workplace transmission
      probability (#DiseaseParm::xmit_work).
*/
template <typename AC, typename ACT, typename ACTD, typename A>
void InteractionModWork<AC,ACT,ACTD,A>::interactAgents(AC& a_agents, /*!< Agent container */
                                                       MultiFab& /*a_mask*/ /*!< Masking behavior */)
{
    BL_PROFILE("InteractionModWork::interactAgents");
    int n_disease = a_agents.numDiseases();

    IntVect bin_size = {AMREX_D_DECL(1, 1, 1)};
    for (int lev = 0; lev < a_agents.numLevels(); ++lev)
    {
        const Geometry& geom = a_agents.Geom(lev);
        const auto dxi = geom.InvCellSizeArray();
        const auto plo = geom.ProbLoArray();
        const auto domain = geom.Domain();

        this->makeBins( a_agents, bin_size, lev, ExaEpi::InteractionNames::work );

#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
        for(MFIter mfi = a_agents.MakeMFIter(lev, TilingIfNotGPU()); mfi.isValid(); ++mfi)
        {
            auto pair_ind = std::make_pair(mfi.index(), mfi.LocalTileIndex());
            auto bins_ptr = a_agents.getBins(pair_ind, ExaEpi::InteractionNames::work);

            auto& ptile = a_agents.ParticlesAt(lev, mfi);
            const auto& ptd = ptile.getParticleTileData();
            auto& aos   = ptile.GetArrayOfStructs();
            const auto np = aos.numParticles();
            auto pstruct_ptr = aos().dataPtr();

            auto binner = GetParticleBin{plo, dxi, domain, bin_size, mfi.validbox()};
            AMREX_ALWAYS_ASSERT(bins_ptr->numBins() >= 0);
            auto inds = bins_ptr->permutationPtr();
            auto offsets = bins_ptr->offsetsPtr();

            for (int d = 0; d < n_disease; d++) {

                auto prob_ptr = this->getAgentProbPtr(a_agents,lev,mfi,d);
                //auto mask_arr = a_mask[mfi].array();
                auto lparm = a_agents.getDiseaseParameters_d(d);

                ParallelForRNG( bins_ptr->numItems(),
                                [=] AMREX_GPU_DEVICE (int ii, RandomEngine const& /*engine*/)
                                noexcept
                {
                    auto i = inds[ii];
                    int i_cell = binner(pstruct_ptr[i]);
                    auto cell_start = offsets[i_cell];
                    auto cell_stop  = offsets[i_cell+1];

                    AMREX_ALWAYS_ASSERT( (Long) i < np);

                    if ( notSusceptible<ACTD>(i, ptd, d) )  { return; }

                    //Real i_mask = mask_arr(home_i_ptr[i], home_j_ptr[i], 0);
                    for (unsigned int jj = cell_start; jj < cell_stop; ++jj) {

                        auto j = inds[jj];
                        AMREX_ALWAYS_ASSERT( (Long) j < np);
                        //Real j_mask = mask_arr(home_i_ptr[j], home_j_ptr[j], 0);
                        if (i == j) continue;

                        if ( isInfectious<ACTD>(j, ptd, d) ) {
                            Real work_scale = 1.0_prt;  // TODO this should vary based on cell
                            binaryInteractionWork<ACTD>( j, i, ptd, lparm, work_scale, prob_ptr );
                        }
                    }
                });
                Gpu::synchronize();
            }
        }
    }
}

#endif
