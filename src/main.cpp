#include <AMReX.H>
#include <AMReX_iMultiFab.H>
#include <AMReX_ParmParse.H>
#include <AMReX_MultiFab.H>

#include "AgentContainer.H"
#include "DemographicData.H"
#include "IO.H"
#include "Utils.H"

using namespace amrex;
using namespace ExaEpi;

void runAgent();

int main (int argc, char* argv[])
{
    amrex::Initialize(argc,argv);

    runAgent();

    amrex::Finalize();
}

/* Determine number of cells in each direction required */
Geometry get_geometry (const DemographicData& demo,
                       const TestParams& params) {
    int is_per[BL_SPACEDIM];
    for (int i = 0; i < BL_SPACEDIM; i++) {
        is_per[i] = true;
    }

    RealBox real_box;
    Box base_domain;
    Geometry geom;

    if (params.ic_type == ICType::Demo) {
        IntVect domain_lo(AMREX_D_DECL(0, 0, 0));
        IntVect domain_hi(AMREX_D_DECL(params.size[0]-1,params.size[1]-1,params.size[2]-1));
        base_domain = Box(domain_lo, domain_hi);

        for (int n = 0; n < BL_SPACEDIM; n++)
        {
            real_box.setLo(n, 0.0);
            real_box.setHi(n, 3000.0);
        }

    } else if (params.ic_type == ICType::Census) {
        IntVect iv;
        iv[0] = iv[1] = (int) std::floor(std::sqrt((double) demo.Ncommunity));
        while (iv[0]*iv[1] <= demo.Ncommunity) {
            ++iv[0];
        }
        base_domain = Box(IntVect(AMREX_D_DECL(0, 0, 0)), iv-1);

        for (int n = 0; n < BL_SPACEDIM; n++)
        {
            real_box.setLo(n, 0.0);
            real_box.setHi(n, 1.0);
        }
    }

    geom.define(base_domain, &real_box, CoordSys::cartesian, is_per);
    return geom;
}

unsigned int **flow;   /* Workerflow matrix */

void read_workerflow (const DemographicData& demo,
                      const TestParams& params,
                      const iMultiFab& unit_mf,
                      const iMultiFab& comm_mf,
                      AgentContainer& pc) {

  /* Allocate worker-flow matrix, only from units with nighttime
     communities on this processor (Unit_on_proc[] flag) */
    flow = (unsigned int **) amrex::The_Arena()->alloc(demo.Nunit*sizeof(unsigned int *));
    for (int i = 0; i < demo.Nunit; i++) {
        if (demo.Unit_on_proc[i]) {
            flow[i] = (unsigned int *) amrex::The_Arena()->alloc(demo.Nunit*sizeof(unsigned int));
            for (int j = 0; j < demo.Nunit; j++) flow[i][j] = 0;
        }
    }

    VisMF::IO_Buffer io_buffer(VisMF::IO_Buffer_Size);

    std::ifstream ifs;
    ifs.rdbuf()->pubsetbuf(io_buffer.dataPtr(), io_buffer.size());

    ifs.open(params.workerflow_filename.c_str(), std::ios::in|std::ios::binary);
    if (!ifs.good()) {
        amrex::FileOpenFailed(params.workerflow_filename);
    }

    const std::streamoff CURPOS = ifs.tellg();
    ifs.seekg(0,std::ios::end);
    const std::streamoff ENDPOS = ifs.tellg();
    const long num_work = (ENDPOS - CURPOS) / (3*sizeof(unsigned int));

    ifs.seekg(CURPOS, std::ios::beg);

    for (int work = 0; work < num_work; ++work) {
        unsigned int from, to, number;
        ifs.read((char*)&from, sizeof(from));
        ifs.read((char*)&to, sizeof(to));
        ifs.read((char*)&number, sizeof(number));
        int i = demo.myIDtoUnit[from];
        if (demo.Unit_on_proc[i]) {
            int j = demo.myIDtoUnit[to];
            if (demo.Start[j+1] != demo.Start[j]) { // if there are communities in this unit
                flow[i][j] = number;
            }
        }
    }

    /* Convert to cumulative numbers to enable random selection */
    for (int i = 0; i < demo.Nunit; i++) {
        if (demo.Unit_on_proc[i]) {
            for (int j = 1; j < demo.Nunit; j++) {
                flow[i][j] += flow[i][j-1];
            }
        }
    }

    /* These numbers were for the true population, and do not include
       the roughly 2% of people who were on vacation or sick during the
       Census 2000 reporting week.  We need to scale the worker flow to
       the model tract residential populations, and might as well add
       the 2% back in while we're at it.... */
    for (int i = 0; i < demo.Nunit; i++) {
        if (demo.Unit_on_proc[i] && demo.Population[i]) {
            unsigned int number = (unsigned int) rint(((double) demo.Population[i]) / 2000.0);
            double scale = 1.02 * (2000.0 * number) / ((double) demo.Population[i]);
            for (int j = 0; j < demo.Nunit; j++) {
                flow[i][j] = rint((double) flow[i][j] * scale);
            }
        }
    }

    const Box& domain = pc.Geom(0).Domain();

    /* This is where workplaces should be assigned */
#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
    for (MFIter mfi(unit_mf, TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        auto& agents_tile = pc.GetParticles(0)[std::make_pair(mfi.index(),mfi.LocalTileIndex())];
        auto& soa = agents_tile.GetStructOfArrays();
        auto age_group_ptr = soa.GetIntData(IntIdx::age_group).data();
        auto home_i_ptr = soa.GetIntData(IntIdx::home_i).data();
        auto home_j_ptr = soa.GetIntData(IntIdx::home_j).data();
        auto work_i_ptr = soa.GetIntData(IntIdx::work_i).data();
        auto work_j_ptr = soa.GetIntData(IntIdx::work_j).data();

        auto np = soa.numParticles();

        auto unit_arr = unit_mf[mfi].array();
        auto comm_arr = comm_mf[mfi].array();

        amrex::ParallelForRNG( np,
            [=] AMREX_GPU_DEVICE (int ip, RandomEngine const& engine) noexcept
            {
                auto from = unit_arr(home_i_ptr[ip], home_j_ptr[ip], 0);

                /* Randomly assign the eligible working-age population */
                unsigned int number = (unsigned int) rint(((Real) demo.Population[from]) / 2000.0);
                unsigned int nwork = 2000.0 * number * .586; /* 58.6% of population is working-age */
                if (nwork == 0) { return; }

                int age_group = age_group_ptr[ip];
                /* Check working-age population */
                if ((age_group == 2) || (age_group == 3)) {
                    unsigned int irnd = amrex::Random_int(nwork, engine);
                    int to, comm_to;
                    if (irnd < flow[from][demo.Nunit-1]) {
                        /* Choose a random destination unit */
                        to = 0;
                        while (irnd >= flow[from][to]) { to++; }
                    }

                    /*If from=to unit, 25% EXTRA chance of working in home community*/
                    if ((from == to) && (amrex::Random(engine) < 0.25)) {
                        comm_to = comm_arr(home_i_ptr[ip], home_j_ptr[ip], 0);
                    } else {
                        /* Choose a random community within that destination unit */
                        comm_to = demo.Start[to] + amrex::Random_int(demo.Start[to+1] - demo.Start[to]);
                        AMREX_ALWAYS_ASSERT(comm_to < demo.Ncommunity);
                    }

                    IntVect comm_to_iv = domain.atOffset(comm_to);
                    work_i_ptr[ip] = comm_to_iv[0];
                    work_j_ptr[ip] = comm_to_iv[1];

                    // still need to handle workgroups
                    /* Number of workgroups,  cd->Nworkgroup */
                    // number = (unsigned int) rint( ((Real) demo.Ndaywork[to]) /
                    //((Real) WG_size * (demo.Start[to+1] - demo.Start[to])) );

                    /* Choose a random workgroup 1, 2, ..., cd->Nworkgroup */
                    //if (number) pt->workgroup = 1 + lrand48() % number;

                    // still need to handle neighborboods
                }
            });
    }
}

void runAgent ()
{
    BL_PROFILE("runAgent");
    TestParams params;
    ExaEpi::Utils::get_test_params(params, "agent");

    DemographicData demo;
    if (params.ic_type == ICType::Census) { demo.InitFromFile(params.census_filename); }
    Geometry geom = get_geometry(demo, params);

    BoxArray ba;
    DistributionMapping dm;
    ba.define(geom.Domain());
    ba.maxSize(params.max_grid_size);
    dm.define(ba);

    iMultiFab num_residents(ba, dm, 6, 0);
    iMultiFab unit_mf(ba, dm, 1, 0);
    iMultiFab FIPS_mf(ba, dm, 2, 0);
    iMultiFab comm_mf(ba, dm, 1, 0);

    AgentContainer pc(geom, dm, ba);

    {
        BL_PROFILE_REGION("Initialization");
        if (params.ic_type == ICType::Demo) {
            pc.initAgentsDemo(num_residents, unit_mf, FIPS_mf, comm_mf, demo);
        } else if (params.ic_type == ICType::Census) {
            pc.initAgentsCensus(num_residents, unit_mf, FIPS_mf, comm_mf, demo);
        }

        read_workerflow(demo, params, unit_mf, comm_mf, pc);
    }

    {
        BL_PROFILE_REGION("Evolution");
        for (int i = 0; i < params.nsteps; ++i)
        {
            amrex::Print() << "Taking step " << i << "\n";

            if (i % 168 == 0) {
                ExaEpi::IO::writePlotFile(pc, num_residents, unit_mf, FIPS_mf, comm_mf, i);
            }  // every week

            pc.updateStatus();
            pc.interactAgents();

            pc.moveAgentsRandomWalk();
            if (i % 24 == 0) { pc.moveRandomTravel(); }  // once a day

            pc.Redistribute();

            pc.printTotals();
        }
    }

    if (params.nsteps % 168 == 0) {
        ExaEpi::IO::writePlotFile(pc, num_residents, unit_mf, FIPS_mf, comm_mf, params.nsteps);
    }
}
