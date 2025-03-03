/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 */
#include <cstdint>
#include <cstring>

#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <adios2.h>

#include <gtest/gtest.h>

std::string engineName; // comes from command line

// Number of elements per process
const std::size_t Nx = 10;
using DataArray = std::array<int32_t, Nx>;

class BPStepsFileLocalArray : public ::testing::Test
{
protected:
    BPStepsFileLocalArray() = default;

    const DataArray I32 = {
        {512, 513, -510, 515, -508, 517, -506, 519, -504, 521}};

    DataArray GenerateData(int step, int rank, int size)
    {
        DataArray d;
        int j = rank + 1 + step * size;
        for (size_t i = 0; i < d.size(); ++i)
        {
            d[i] = I32[i] + j;
        }
        return d;
    }

    std::string ArrayToString(int32_t *data, size_t nelems)
    {
        std::stringstream ss;
        ss << "[";
        for (size_t i = 0; i < nelems; ++i)
        {
            ss << data[i];
            if (i < nelems - 1)
            {
                ss << " ";
            }
        }
        ss << "]";
        return ss.str();
    }
};

enum class ReadMode
{
    ReadFileStepByStepBlocks,
    ReadStreamBlocks
};

std::string ReadModeToString(const ReadMode r)
{
    switch (r)
    {
    case ReadMode::ReadFileStepByStepBlocks:
        return "ReadFileStepByStepBlocks";
    case ReadMode::ReadStreamBlocks:
        return "ReadStreamBlocks";
    }
    return "unknown";
}

class BPStepsFileLocalArrayReaders
: public BPStepsFileLocalArray,
  public ::testing::WithParamInterface<ReadMode>
{
protected:
    ReadMode GetReadMode() { return GetParam(); };
};

// Basic case: Variable written every step
TEST_P(BPStepsFileLocalArrayReaders, EveryStep)
{
    const ReadMode readMode = GetReadMode();
    std::string fname_prefix =
        "BPStepsFileLocalArray.EveryStep." + ReadModeToString(readMode);
    int mpiRank = 0, mpiSize = 1;
    const std::size_t NSteps = 4;

#if ADIOS2_USE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
    MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
#endif

    DataArray m_TestData[NSteps];
    adios2::Dims shape{};
    adios2::Dims start{};
    adios2::Dims count{Nx};

    std::string fname;
#if ADIOS2_USE_MPI
    adios2::ADIOS adios(MPI_COMM_WORLD);
    fname = fname_prefix + ".MPI.bp";
#else
    adios2::ADIOS adios;
    fname = fname_prefix + ".Serial.bp";
#endif

    // Write test data using ADIOS2
    {
        if (!mpiRank)
        {
            std::cout << "Write one variable in every step" << std::endl;
        }
        adios2::IO io = adios.DeclareIO("Write");
        if (!engineName.empty())
        {
            io.SetEngine(engineName);
        }

        adios2::Engine engine = io.Open(fname, adios2::Mode::Write);

        auto var_i32 = io.DefineVariable<int32_t>("i32", shape, start, count);

        for (int step = 0; step < static_cast<int>(NSteps); ++step)
        {
            // Generate test data for each process uniquely
            m_TestData[step] = GenerateData(step, mpiRank, mpiSize);
            std::cout << "Rank " << mpiRank << " write step " << step << ": "
                      << ArrayToString(m_TestData[step].data(), Nx)
                      << std::endl;
            engine.BeginStep();
            engine.Put(var_i32, m_TestData[step].data());
            engine.EndStep();
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    adios2::IO io = adios.DeclareIO("Read");
    if (!engineName.empty())
    {
        io.SetEngine(engineName);
    }
    if (readMode == ReadMode::ReadFileStepByStepBlocks)
    {
        /// Read back data with File reading mode
        /// Read back step by step and block by block and check data
        adios2::Engine engine = io.Open(fname, adios2::Mode::ReadRandomAccess);
        EXPECT_TRUE(engine);

        if (!mpiRank)
        {
            std::cout << "Read with File reading mode, read step by step, "
                         "block by block"
                      << std::endl;
        }
        auto var_i32 = io.InquireVariable<int32_t>("i32");
        EXPECT_TRUE(var_i32);
        EXPECT_EQ(var_i32.Steps(), NSteps);
        EXPECT_EQ(var_i32.StepsStart(), 0);
        for (size_t step = 0; step < NSteps; ++step)
        {
            var_i32.SetStepSelection({step, 1});
            size_t blockID = static_cast<size_t>(mpiRank);
            var_i32.SetBlockSelection(blockID);
            DataArray d;
            engine.Get(var_i32, d.data(), adios2::Mode::Sync);
            std::cout << "Rank " << mpiRank << " read step " << step
                      << " block " << blockID << ": "
                      << ArrayToString(d.data(), Nx) << std::endl;
            auto start = var_i32.Start();
            auto count = var_i32.Count();
            EXPECT_EQ(start.size(), 0);
            EXPECT_EQ(count.size(), 1);
            EXPECT_EQ(count[0], 1 * Nx);
            for (size_t i = 0; i < Nx; ++i)
            {
                EXPECT_EQ(d[i], m_TestData[step][i]);
            }
        }
        engine.Close();
    }
    else if (readMode == ReadMode::ReadStreamBlocks)
    {
        /// Read back data with Stream reading mode
        /// Read back step by step and check data
        adios2::Engine engine = io.Open(fname, adios2::Mode::Read);
        EXPECT_TRUE(engine);

        if (!mpiRank)
        {
            std::cout << "Read with Stream reading mode, read step by step, "
                         "block by block"
                      << std::endl;
        }
        for (size_t step = 0; step < NSteps; ++step)
        {
            engine.BeginStep();
            auto var_i32 = io.InquireVariable<int32_t>("i32");
            EXPECT_TRUE(var_i32);
            // EXPECT_EQ(var_i32.Steps(), 1);
            EXPECT_EQ(var_i32.StepsStart(), 0);
            size_t blockID = static_cast<size_t>(mpiRank);
            var_i32.SetBlockSelection(blockID);
            DataArray d;
            engine.Get(var_i32, d.data(), adios2::Mode::Sync);
            std::cout << "Rank " << mpiRank << " read step " << step
                      << " block " << blockID << ": "
                      << ArrayToString(d.data(), Nx) << std::endl;
            auto start = var_i32.Start();
            auto count = var_i32.Count();
            EXPECT_EQ(start.size(), 0);
            EXPECT_EQ(count.size(), 1);
            EXPECT_EQ(count[0], 1 * Nx);
            for (size_t i = 0; i < Nx; ++i)
            {
                EXPECT_EQ(d[i], m_TestData[step][i]);
            }
            engine.EndStep();
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
}

// Variable written every other step from 2nd step
TEST_P(BPStepsFileLocalArrayReaders, NewVarPerStep)
{
    const ReadMode readMode = GetReadMode();
    std::string fname_prefix =
        "BPStepsFileLocalArray.NewVarPerStep." + ReadModeToString(readMode);
    int mpiRank = 0, mpiSize = 1;
    const std::size_t NSteps = 4;

#if ADIOS2_USE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
    MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
#endif

    DataArray m_TestData[NSteps];
    adios2::Dims shape{};
    adios2::Dims start{};
    adios2::Dims count{Nx};

    std::string fname;
#if ADIOS2_USE_MPI
    adios2::ADIOS adios(MPI_COMM_WORLD);
    fname = fname_prefix + ".MPI.bp";
#else
    adios2::ADIOS adios;
    fname = fname_prefix + ".Serial.bp";
#endif

    auto lf_VarName = [](int step) -> std::string {
        return "i32_" + std::to_string(step);
    };

    // Write test data using ADIOS2
    {
        if (!mpiRank)
        {
            std::cout << "Write a new variable in each step" << std::endl;
        }
        adios2::IO io = adios.DeclareIO("Write");
        if (!engineName.empty())
        {
            io.SetEngine(engineName);
        }

        adios2::Engine engine = io.Open(fname, adios2::Mode::Write);

        for (int step = 0; step < static_cast<int>(NSteps); ++step)
        {
            const std::string varName = lf_VarName(step);
            auto var = io.DefineVariable<int32_t>(varName, shape, start, count);
            // Generate test data for each process uniquely
            m_TestData[step] = GenerateData(step, mpiRank, mpiSize);
            std::cout << "Rank " << mpiRank << " write step " << step << " var "
                      << varName << ": "
                      << ArrayToString(m_TestData[step].data(), Nx)
                      << std::endl;
            engine.BeginStep();
            engine.Put(var, m_TestData[step].data());
            engine.EndStep();
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    adios2::IO io = adios.DeclareIO("Read");
    if (!engineName.empty())
    {
        io.SetEngine(engineName);
    }
    if (readMode == ReadMode::ReadFileStepByStepBlocks)
    {
        /// Read back each variable with File reading mode
        /// Read back block by block and check data
        adios2::Engine engine = io.Open(fname, adios2::Mode::ReadRandomAccess);
        EXPECT_TRUE(engine);

        if (!mpiRank)
        {
            std::cout
                << "Read with File reading mode using explicit SetStepSelection"
                   ", block by block"
                << std::endl;
        }
        for (int step = 0; step < static_cast<int>(NSteps); ++step)
        {
            const std::string varName = lf_VarName(step);
            auto var = io.InquireVariable<int32_t>(varName);
            EXPECT_TRUE(var);
            EXPECT_EQ(var.Steps(), 1);
            EXPECT_EQ(var.StepsStart(), 0);
            var.SetStepSelection({0, 1});
            size_t blockID = static_cast<size_t>(mpiRank);
            var.SetBlockSelection(blockID);
            DataArray d;
            engine.Get(var, d.data(), adios2::Mode::Sync);
            std::cout << "Rank " << mpiRank << " read step " << step
                      << " block " << blockID << ": "
                      << ArrayToString(d.data(), Nx) << std::endl;
            //  not ok on all engines
            //            auto start = var.Start();
            //            auto count = var.Count();
            //            EXPECT_EQ(start.size(), 0);
            //            EXPECT_EQ(count.size(), 1);
            //            EXPECT_EQ(count[0], 1 * Nx);
            for (size_t i = 0; i < Nx; ++i)
            {
                EXPECT_EQ(d[i], m_TestData[step][i]);
            }
        }
        engine.Close();
    }
    else if (readMode == ReadMode::ReadStreamBlocks)
    {
        /// Read back each variable with Streaming reading mode
        adios2::Engine engine = io.Open(fname, adios2::Mode::Read);
        EXPECT_TRUE(engine);

        if (!mpiRank)
        {
            std::cout
                << "Read with Stream reading mode step by step, block by block"
                << std::endl;
        }
        for (int step = 0; step < static_cast<int>(NSteps); ++step)
        {
            engine.BeginStep();
            const std::string varName = lf_VarName(step);
            auto var = io.InquireVariable<int32_t>(varName);
            EXPECT_TRUE(var);
            EXPECT_EQ(var.Steps(), 1);
            EXPECT_EQ(var.StepsStart(), 0);
            size_t blockID = static_cast<size_t>(mpiRank);
            var.SetBlockSelection(blockID);
            DataArray d;
            engine.Get(var, d.data(), adios2::Mode::Sync);
            std::cout << "Rank " << mpiRank << " read step " << step
                      << " block " << blockID << ": "
                      << ArrayToString(d.data(), Nx) << std::endl;
            //  not ok on all engines
            //            auto start = var.Start();
            //            auto count = var.Count();
            //            EXPECT_EQ(start.size(), 0);
            //            EXPECT_EQ(count.size(), 1);
            //            EXPECT_EQ(count[0], 1 * Nx);
            for (size_t i = 0; i < Nx; ++i)
            {
                EXPECT_EQ(d[i], m_TestData[step][i]);
            }
            engine.EndStep();
#if ADIOS2_USE_MPI
            MPI_Barrier(MPI_COMM_WORLD);
#endif
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
}

INSTANTIATE_TEST_SUITE_P(BPStepsFileLocalArray, BPStepsFileLocalArrayReaders,
                         ::testing::Values(ReadMode::ReadFileStepByStepBlocks,
                                           ReadMode::ReadStreamBlocks));

class BPStepsFileLocalArrayParameters
: public BPStepsFileLocalArray,
  public ::testing::WithParamInterface<std::tuple<size_t, size_t, ReadMode>>
{
protected:
    size_t GetNsteps() { return std::get<0>(GetParam()); };
    size_t GetOddity() { return std::get<1>(GetParam()); };
    ReadMode GetReadMode() { return std::get<2>(GetParam()); };
};

// Variable written every other step from 1st step
TEST_P(BPStepsFileLocalArrayParameters, EveryOtherStep)
{
    const std::size_t NSteps = GetNsteps();
    const std::size_t Oddity = GetOddity();
    const ReadMode readMode = GetReadMode();
    std::string fname_prefix =
        "BPStepsFileLocalArray.EveryOtherStep.Steps" + std::to_string(NSteps) +
        ".Oddity" + std::to_string(Oddity) + "." + ReadModeToString(readMode);
    int mpiRank = 0, mpiSize = 1;

#if ADIOS2_USE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
    MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
#endif

    std::vector<DataArray> m_TestData;
    adios2::Dims shape{};
    adios2::Dims start{};
    adios2::Dims count{Nx};

    std::string fname;
#if ADIOS2_USE_MPI
    adios2::ADIOS adios(MPI_COMM_WORLD);
    fname = fname_prefix + ".MPI.bp";
#else
    adios2::ADIOS adios;
    fname = fname_prefix + ".Serial.bp";

#endif

    size_t stepsWritten = 0;

    // Write test data using ADIOS2
    {
        if (!mpiRank)
        {
            std::cout << "Write one variable in every "
                      << (Oddity ? "ODD" : "EVEN") << " steps, within "
                      << std::to_string(NSteps) << " steps" << std::endl;
        }
        adios2::IO io = adios.DeclareIO("Write");
        if (!engineName.empty())
        {
            io.SetEngine(engineName);
        }

        adios2::Engine engine = io.Open(fname, adios2::Mode::Write);

        auto var_i32 = io.DefineVariable<int32_t>("i32", shape, start, count);
        auto var_step = io.DefineVariable<int>("step");
        for (int step = 0; step < static_cast<int>(NSteps); ++step)
        {
            // Generate test data for each process uniquely
            engine.BeginStep();
            engine.Put(var_step, step);
            if (step % 2 == static_cast<int>(Oddity))
            {
                m_TestData.push_back(GenerateData(step, mpiRank, mpiSize));
                std::cout << "Rank " << mpiRank << " write step " << step
                          << ": "
                          << ArrayToString(m_TestData[stepsWritten].data(), Nx)
                          << std::endl;
                engine.Put(var_i32, m_TestData[stepsWritten].data());
                ++stepsWritten;
            }
            engine.EndStep();
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    adios2::IO io = adios.DeclareIO("Read");
    if (!engineName.empty())
    {
        io.SetEngine(engineName);
    }
    if (readMode == ReadMode::ReadFileStepByStepBlocks)
    {
        adios2::Engine engine = io.Open(fname, adios2::Mode::ReadRandomAccess);
        EXPECT_TRUE(engine);

        /// Read back data with File reading mode
        /// Read back step by step, block by block and check data
        if (!mpiRank)
        {
            std::cout << "Read with File reading mode, read step by step, "
                         "block by block"
                      << std::endl;
        }

        auto var_i32 = io.InquireVariable<int32_t>("i32");
        EXPECT_TRUE(var_i32);
        EXPECT_EQ(var_i32.Steps(), stepsWritten);
        EXPECT_EQ(var_i32.StepsStart(), 0);

        for (size_t s = 0; s < stepsWritten; ++s)
        {
            var_i32.SetStepSelection({s, 1});
            size_t blockID = static_cast<size_t>(mpiRank);
            var_i32.SetBlockSelection(blockID);
            DataArray d;
            engine.Get(var_i32, d.data(), adios2::Mode::Sync);
            std::cout << "Rank " << mpiRank << " read step " << s << " block "
                      << blockID << ": " << ArrayToString(d.data(), Nx)
                      << std::endl;
            //  not ok on all engines
            //            auto start = var_i32.Start();
            //            auto count = var_i32.Count();
            //            EXPECT_EQ(start.size(), 0);
            //            EXPECT_EQ(count.size(), 1);
            //            EXPECT_EQ(count[0], 1 * Nx);
            for (size_t i = 0; i < Nx; ++i)
            {
                EXPECT_EQ(d[i], m_TestData[s][i]);
            }
        }
        engine.Close();
    }
    else if (readMode == ReadMode::ReadStreamBlocks)
    {
        /// Read back data with Stream reading mode
        /// Read back step by step and check data
        adios2::Engine engine = io.Open(fname, adios2::Mode::Read);
        EXPECT_TRUE(engine);

        if (!mpiRank)
        {
            std::cout
                << "Read with Stream reading mode step by step, block by block"
                << std::endl;
        }

        size_t writtenStep = 0;
        for (size_t step = 0; step < NSteps; ++step)
        {
            engine.BeginStep();
            if (step % 2 == Oddity)
            {
                auto var_i32 = io.InquireVariable<int32_t>("i32");
                EXPECT_TRUE(var_i32);
                // EXPECT_EQ(var_i32.Steps(), 1);
                EXPECT_EQ(var_i32.StepsStart(), 0);
                size_t blockID = static_cast<size_t>(mpiRank);
                var_i32.SetBlockSelection(blockID);
                DataArray d;
                engine.Get(var_i32, d.data(), adios2::Mode::Sync);
                std::cout << "Rank " << mpiRank << " read step " << step
                          << " block " << blockID << ": "
                          << ArrayToString(d.data(), Nx) << std::endl;
                //  not ok on all engines
                //                auto start = var_i32.Start();
                //                auto count = var_i32.Count();
                //                EXPECT_EQ(start.size(), 0);
                //                EXPECT_EQ(count.size(), 1);
                //                EXPECT_EQ(count[0], 1 * Nx);
                for (size_t i = 0; i < Nx; ++i)
                {
                    EXPECT_EQ(d[i], m_TestData[writtenStep][i]);
                }
                ++writtenStep;
            }
            engine.EndStep();
#if ADIOS2_USE_MPI
            MPI_Barrier(MPI_COMM_WORLD);
#endif
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
}

INSTANTIATE_TEST_SUITE_P(
    BPStepsFileLocalArray, BPStepsFileLocalArrayParameters,
    ::testing::Values(std::make_tuple(4, 0, ReadMode::ReadFileStepByStepBlocks),
                      std::make_tuple(4, 0, ReadMode::ReadStreamBlocks),
                      std::make_tuple(4, 1, ReadMode::ReadFileStepByStepBlocks),
                      std::make_tuple(4, 1, ReadMode::ReadStreamBlocks),
                      std::make_tuple(2, 1, ReadMode::ReadFileStepByStepBlocks),
                      std::make_tuple(2, 1, ReadMode::ReadStreamBlocks)));
//******************************************************************************
// main
//******************************************************************************

int main(int argc, char **argv)
{
#if ADIOS2_USE_MPI
    int provided;
    MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &provided);
#endif

    int result;
    ::testing::InitGoogleTest(&argc, argv);

    if (argc > 1)
    {
        engineName = std::string(argv[1]);
    }
    result = RUN_ALL_TESTS();

#if ADIOS2_USE_MPI
    MPI_Finalize();
#endif

    return result;
}
