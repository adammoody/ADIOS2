#include <cstdint>

#include <iostream>
#include <stdexcept>

#include <adios2.h>

#include <gtest/gtest.h>

#define str_helper(X) #X
#define str(X) str_helper(X)

class YAMLConfigTest : public ::testing::Test
{
public:
    YAMLConfigTest() : configDir(str(YAML_CONFIG_DIR)) {}

    std::string configDir;
};

TEST_F(YAMLConfigTest, TwoIOs)
{
    const std::string configFile(
        configDir + std::string(&adios2::PathSeparator, 1) + "config1.yaml");

#if ADIOS2_USE_MPI
    adios2::ADIOS adios(configFile, MPI_COMM_WORLD);
#else
    adios2::ADIOS adios(configFile);
#endif

    // must be declared at least once
    EXPECT_THROW(adios2::IO io = adios.AtIO("Test IO 1");
                 (void)io, std::invalid_argument);

    EXPECT_NO_THROW({
        adios2::IO io = adios.DeclareIO("Test IO 1");
        const adios2::Params params = io.Parameters();
        ASSERT_EQ(params.size(), 5);
        EXPECT_THROW((void)params.at("DoesNotExist"), std::out_of_range);
        EXPECT_EQ(params.at("Threads"), "1");
        EXPECT_EQ(params.at("ProfileUnits"), "Microseconds");
        EXPECT_EQ(params.at("MaxBufferSize"), "20Mb");
        EXPECT_EQ(params.at("InitialBufferSize"), "1Mb");
        EXPECT_EQ(params.at("BufferGrowthFactor"), "2");
        adios2::Engine engine =
            io.Open("Test BP Writer 1", adios2::Mode::Write);
        engine.Close();
    });
    EXPECT_NO_THROW(adios2::IO io = adios.AtIO("Test IO 1"); (void)io);

    EXPECT_THROW(adios2::IO io = adios.AtIO("Test IO 2");
                 (void)io, std::invalid_argument);
    EXPECT_NO_THROW({
        adios2::IO io = adios.DeclareIO("Test IO 2");
        const adios2::Params params = io.Parameters();
        ASSERT_EQ(params.size(), 0);
    });
    EXPECT_NO_THROW(adios.AtIO("Test IO 2"));

    // double declaring
    EXPECT_THROW(adios.DeclareIO("Test IO 1"), std::invalid_argument);
    EXPECT_THROW(adios.DeclareIO("Test IO 2"), std::invalid_argument);
}

TEST_F(YAMLConfigTest, OpTypeException)
{
    const std::string configFile(configDir +
                                 std::string(&adios2::PathSeparator, 1) +
                                 "configOpTypeException.yaml");

#if ADIOS2_USE_MPI
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == 0)
    {
        EXPECT_THROW(adios2::ADIOS adios(configFile, MPI_COMM_SELF),
                     std::invalid_argument);
    }
#else
    EXPECT_THROW(adios2::ADIOS adios(configFile), std::invalid_argument);
#endif
}

TEST_F(YAMLConfigTest, OpNullException)
{
    const std::string configFile(configDir +
                                 std::string(&adios2::PathSeparator, 1) +
                                 "configOpNullException.yaml");

#if ADIOS2_USE_MPI
    EXPECT_THROW(adios2::ADIOS adios(configFile, MPI_COMM_WORLD),
                 std::invalid_argument);
#else
    EXPECT_THROW(adios2::ADIOS adios(configFile), std::invalid_argument);
#endif
}

int main(int argc, char **argv)
{
#if ADIOS2_USE_MPI
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
#endif

    int result;
    ::testing::InitGoogleTest(&argc, argv);
    result = RUN_ALL_TESTS();

#if ADIOS2_USE_MPI
    MPI_Finalize();
#endif

    return result;
}
