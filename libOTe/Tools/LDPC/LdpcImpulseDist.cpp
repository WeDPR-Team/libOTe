
#include "LdpcImpulseDist.h"
#include "LdpcDecoder.h"
#include "Util.h"
#include <unordered_set>
#include <numeric>
#include "LdpcSampler.h"
#include "cryptoTools/Common/Timer.h"
#include "cryptoTools/Crypto/PRNG.h"
#include "libOTe/Tools/LDPC/LdpcEncoder.h"
extern "C" {
#include "libOTe/Tools/LDPC/Algo994/data_defs.h"
}

#include <thread>
#include <fstream>

#include <iomanip> // put_time

namespace osuCrypto
{


    struct ListIter
    {
        std::vector<u64> set;

        ListDecoder mType;
        u64 mTotalI = 0, mTotalEnd;
        u64 mI = 0, mEnd = 1, mN = 0, mCurWeight = 0;

        u64 mD = 1;
        void initChase(u64 d)
        {
            assert(d < 24);
            mType = ListDecoder::Chase;
            mD = d;
            mI = 0;
            ++(*this);
        }

        void initOsd(u64 d, u64 n, bool startAtZero)
        {
            assert(d < 24);
            mType = ListDecoder::OSD;

            mN = n;
            mTotalI = 0;
            mTotalEnd = 1ull << d;

            mCurWeight = startAtZero ? 0 : 1;
            mI = 0;
            mEnd = choose(n, mCurWeight);

            set = ithCombination(mI, n, mCurWeight);
        }


        void operator++()
        {
            assert(*this);
            if (mType == ListDecoder::Chase)
            {
                set.clear();
                ++mI;
                oc::BitIterator ii((u8*)&mI);
                for (u64 i = 0; i < mD; ++i)
                {
                    if (*ii)
                        set.push_back(i);
                    ++ii;
                }
            }
            else
            {

                ++mI;
                ++mTotalI;

                if (mI == mEnd)
                {
                    mI = 0;
                    ++mCurWeight;
                    mEnd = choose(mN, mCurWeight);
                }

                if (mN >= mCurWeight)
                {
                    set.resize(mCurWeight);
                    ithCombination(mI, mN, set);
                }
                else
                    set.clear();
            }
        }

        std::vector<u64>& operator*()
        {
            return set;
        }


        operator bool() const
        {
            if (mType == ListDecoder::Chase)
                return mI != (1ull << mD);
            else
            {
                return mTotalI != mTotalEnd && mCurWeight <= mN;
            }
        }
    };


    template <typename T>
    void sort_indexes(span<const T> v, span<u64> idx) {

        // initialize original index locations
        assert(v.size() == idx.size());
        std::iota(idx.begin(), idx.end(), 0);

        //std::partial_sort()
        std::stable_sort(idx.begin(), idx.end(),
            [&v](size_t i1, size_t i2) {return v[i1] < v[i2]; });
    }

    std::mutex minWeightMtx;
    u32 minWeight(0);
    std::vector<u8> minCW;

    std::unordered_set<u64> heatSet;
    std::vector<u64> heatMap;
    std::vector<u64> heatMapCount;
    u64 nextTimeoutIdx;

    struct Worker
    {
        std::vector<double> llr;
        std::vector<u8> y;
        std::vector<u64> llrSetList;
        std::vector<u8> codeword;
        std::vector<u64> sortIdx, permute, weights;
        std::vector<std::vector<u64>> backProps;
        LdpcDecoder D;
        DynSparseMtx H;
        DenseMtx DH;
        std::vector<u64> dSet, eSet;
        std::unordered_set<u64> eeSet;
        bool verbose = false;
        BPAlgo algo = BPAlgo::LogBP;
        ListDecoder listDecoder = ListDecoder::OSD;
        u64 Ng = 3;
        oc::PRNG prng;
        bool abs = false;
        double timeout;

        void impulseDist(u64 i, u64 k, u64 Nd, u64 maxIter, bool randImpulse)
        {
            if (prng.mBufferByteCapacity == 0)
                prng.SetSeed(oc::sysRandomSeed());

            auto n = D.mH.cols();
            auto m = D.mH.rows();

            llr.resize(n);// , lr.resize(n);
            y.resize(m);
            codeword.resize(n);
            sortIdx.resize(n);
            backProps.resize(m);
            weights.resize(n);

            //auto p = 0.9999;
            auto llr0 = encodeLLR(0.501, 0);
            auto llr1 = encodeLLR(0.999, 1);
            //auto lr0 = encodeLR(0.501, 0);
            //auto lr1 = encodeLR(0.9999999, 1);

            std::fill(llr.begin(), llr.end(), llr0);
            std::vector<u64> impulse;
            if (randImpulse)
            {
                std::set<u64> set;
                //u64 w = prng.get();
                //w = (w % (k)) + 1;
                //assert(k + 1 < n);
                while (set.size() != k)
                {
                    auto i = prng.get<u64>() % n;
                    set.insert(i);
                }
                impulse.insert(impulse.end(), set.begin(), set.end());
            }
            else
            {
                impulse = ithCombination(i, n, k);

            }

            for (auto i : impulse)
                llr[i] = llr1;

            switch (algo)
            {
            case BPAlgo::LogBP:
                D.logbpDecode2(llr, maxIter);
                break;
            case BPAlgo::AltLogBP:
                D.altDecode(llr, false, maxIter);
                break;
            case BPAlgo::MinSum:
                D.altDecode(llr, true, maxIter);
                break;
            default:
                std::cout << "bad algo " << (int)algo << std::endl;
                std::abort();
                break;
            }
            //bpDecode(lr, maxIter);
            //for (auto& l : mL)
            for (u64 i = 0; i < n; ++i)
            {
                if (abs)
                    llr[i] = std::abs(D.mL[i]);
                else
                    llr[i] = (D.mL[i]);
            }

            sort_indexes<double>(llr, sortIdx);


            u64 ii = 0;
            dSet.clear();
            eSet.clear();

            bool sparse = false;
            if (sparse)
                H = D.mH;
            else
                DH = D.mH.dense();

            VecSortSet col;

            while (ii < n && eSet.size() < m)
            {
                auto c = sortIdx[ii++];

                if (sparse)
                    col = H.col(c);
                else
                    DH.colIndexSet(c, col.mData);

                bool set = false;

                for (auto r : col)
                {
                    if (r >= eSet.size())
                    {
                        if (set)
                        {
                            if (sparse)
                                H.rowAdd(r, eSet.size());
                            else
                                DH.row(r) ^= DH.row(eSet.size());
                            //assert(H(r, c) == 0);
                        }
                        else
                        {
                            set = true;
                            if (sparse)
                                H.rowSwap(eSet.size(), r);
                            else
                                DH.row(eSet.size()).swap(DH.row(r));
                        }
                    }
                }

                if (set == false)
                {
                    dSet.push_back(c);
                }
                else
                {
                    eSet.push_back(c);
                }
            }

            if (!sparse)
                H = DH;


            //auto HH1 = H.sparse().dense().gausianElimination();
            //auto HH2 = mH.dense().gausianElimination();
            //assert(HH1 == HH2);


            if (eSet.size() != m)
            {
                std::cout << "bad eSet size " << LOCATION << std::endl;
                abort();
            }

            auto gap = dSet.size();

            while (dSet.size() < Nd)
            {
                auto col = sortIdx[ii++];
                dSet.push_back(col);
            }

            permute = eSet;
            permute.insert(permute.end(), dSet.begin(), dSet.end());
            permute.insert(permute.end(), sortIdx.begin() + permute.size(), sortIdx.end());
            //if (v)
            //{

            //    auto H2 = H.selectColumns(permute);

            //    std::cout << " " << gap << "\n" << H2 << std::endl;
            //    //for (auto l : mL)

            //    for (u64 i = 0; i < n; ++i)
            //    {
            //        std::cout << decodeLLR(D.mL[permute[i]]) << " ";
            //    }
            //    std::cout << std::endl;
            //}

            std::fill(y.begin(), y.end(), 0);

            llrSetList.clear();

            for (u64 i = m; i < n; ++i)
            {
                auto col = permute[i];
                codeword[col] = decodeLLR(D.mL[col]);


                if (codeword[col])
                {
                    llrSetList.push_back(col);
                    for (auto row : H.col(col))
                    {
                        y[row] ^= 1;
                    }
                }
            }

            eeSet.clear();
            eeSet.insert(eSet.begin(), eSet.end());
            for (u64 i = 0; i < m - 1; ++i)
            {
                backProps[i].clear();
                for (auto c : H.row(i))
                {
                    if (c != permute[i] &&
                        eeSet.find(c) != eeSet.end())
                    {
                        backProps[i].push_back(c);
                    }
                }
            }



            ListIter cIter;

            if (listDecoder == ListDecoder::Chase)
                cIter.initChase(Nd);
            else
                cIter.initOsd(Nd, std::min(Ng, n - m), llrSetList.size() > 0);

            //u32 s = 0;
            //u32 e = 1 << Nd;
            while (cIter)
            {

                //for (u64 i = m + Nd; i < n; ++i)
                //{
                //    auto col = permute[i];
                //    codeword[col] = y[i];
                //}

                ////yp = y;
                std::fill(codeword.begin(), codeword.end(), 0);


                // check if BP resulted in any
                // of the top bits being set to 1.
                // if so, preload the partially 
                // solved solution for this.
                if (llrSetList.size())
                {
                    for (u64 i = 0; i < m; ++i)
                        codeword[permute[i]] = y[i];
                    for (auto i : llrSetList)
                        codeword[i] = 1;
                }

                // next, iterate over setting some 
                // of the remaining bits. 
                auto& oneSet = *cIter;
                //for (u64 i = m; i < m + Nd; ++i, ++iter)
                for (auto i : oneSet)
                {

                    auto col = permute[i + m];

                    // check if this was not already set to
                    // 1 by the BP.
                    if (codeword[col] == 0)
                    {
                        codeword[col] = 1;
                        for (auto row : H.col(col))
                        {
                            codeword[permute[row]] ^= 1;
                        }
                    }
                }

                ++cIter;

                // now perform back prop on the remaining 
                // postions.
                for (u64 i = m - 1; i != ~0ull; --i)
                {
                    for (auto c : backProps[i])
                    {
                        if (codeword[c])
                            codeword[permute[i]] ^= 1;
                    }
                }


                // check if its a codework (should always be one)
                if (D.check(codeword) == false) {
                    std::cout << "bad codeword " << LOCATION << std::endl;
                    abort();
                }

                // record the weight.
                auto w = std::accumulate(codeword.begin(), codeword.end(), 0ull);
                ++weights[w];

                if (w && w < minWeight + 10)
                {

                    oc::RandomOracle ro(sizeof(u64));
                    ro.Update(codeword.data(), codeword.size());
                    u64 h;
                    ro.Final(h);

                    std::lock_guard<std::mutex> lock(minWeightMtx);

                    if (w < minWeight)
                    {
                        if (verbose)
                            std::cout << " w=" << w << std::flush;

                        minWeight = w;
                        minCW.clear();
                        minCW.insert(minCW.end(), codeword.begin(), codeword.end());

                        if (timeout > 0)
                        {
                            u64 nn = (i + 1) * timeout;
                            nextTimeoutIdx = std::max(nextTimeoutIdx, nn);
                        }
                    }



                    if (heatSet.find(h) == heatSet.end())
                    {
                        heatSet.insert(h);
                        //heatMap[w].resize(n);
                        for (u64 i = 0; i < n; ++i)
                        {
                            if (codeword[i])
                                ++heatMap[i];
                            ++heatMapCount[w];
                        }
                    }

                }
            }
        }

    };




    u64 impulseDist(
        SparseMtx& mH,
        u64 Nd, u64 Ng,
        u64 w,
        u64 maxIter, u64 nt, bool randImpulse, u64 trials, BPAlgo algo,
        ListDecoder listDecoder, bool verbose, double timeout)
    {
        assert(Nd < 32);
        auto n = mH.cols();
        auto m = mH.rows();

        LdpcDecoder D;
        D.init(mH);
        minWeight = 999999999;
        minCW.clear();


        nt = nt ? nt : 1;
        std::vector<Worker> wrks(nt);

        for (auto& ww : wrks)
        {
            ww.algo = algo;
            ww.D.init(mH);
            ww.verbose = verbose;

            ww.Ng = Ng;
            ww.listDecoder = listDecoder;
            ww.timeout = timeout;
        }
        nextTimeoutIdx = 0;
        if (randImpulse)
        {
            std::vector<std::thread> thrds(nt);

            for (u64 t = 0; t < nt; ++t)
            {
                thrds[t] = std::thread([&, t]() {

                    for (u64 i = t; i < trials; i += nt)
                    {
                        wrks[t].impulseDist(i, w, Nd, maxIter, randImpulse);
                    }
                    });
            }

            for (u64 t = 0; t < nt; ++t)
                thrds[t].join();
        }
        else
        {

            bool timedOut = false;
            std::vector<std::thread> thrds(nt);

            for (u64 t = 0; t < nt; ++t)
            {
                thrds[t] = std::thread([&, t]() {

                    u64 ii = t;
                    for (u64 k = 0; k < w + 1; ++k)
                    {

                        auto f = choose(n, k);
                        for (; ii < f && timedOut == false; ii += nt)
                        {

                            wrks[t].impulseDist(ii, k, Nd, maxIter, randImpulse);


                            if (k == w && (ii % 100) == 0)
                            {
                                std::lock_guard<std::mutex> lock(minWeightMtx);

                                if (nextTimeoutIdx && nextTimeoutIdx < ii)
                                    timedOut = true;
                            }
                        }

                        ii -= f;
                    }
                    }
                );
            }

            for (u64 t = 0; t < nt; ++t)
                thrds[t].join();

        }

        std::vector<u64> weights(n);
        for (u64 t = 0; t < nt; ++t)
        {
            for (u64 i = 0; i < wrks[t].weights.size(); ++i)
                weights[i] += wrks[t].weights[i];
        }

        auto i = 1;
        while (weights[i] == 0) ++i;
        auto ret = i;

        if (verbose)
        {
            std::cout << std::endl;
            auto j = 0;
            while (j++ < 10)
            {
                std::cout << i << " " << weights[i] << std::endl;
                ++i;
            }
        }

        return ret;
    }

    std::string return_current_time_and_date()
    {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);

        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%c %X");
        return ss.str();
    }

    void tests::LdpcDecode_impulse_test(const oc::CLP& cmd)
    {
        // general parameter
        auto rowVec = cmd.getManyOr<u64>("r", { {50} });
        double e = cmd.getOr("e", 2.0);
        //u64 cols = rows * e;
        u64 trial = cmd.getOr("trials", 1);
        u64 tStart = cmd.getOr("tStart", 0);
        u64 seed = cmd.getOr("seed", 0);
        bool verbose = cmd.isSet("v");

        // TZ+ parameters
        u64 colWeight = cmd.getOr("cw", 5);
        u64 dWeight = cmd.getOr("dw", 2);
        u64 gap = cmd.getOr("g", 1);
        bool uniform = cmd.isSet("u");

        u64 diag = cmd.getOr("diag", 0);
        u64 dDiag = cmd.getOr("dDiag", 0);
        auto doubleBand = cmd.getMany<double>("db");
        bool trim = cmd.isSet("trim");
        bool extend = cmd.isSet("extend");

        bool hm = cmd.isSet("hm");

        std::string logPath = cmd.getOr<std::string>("log", "");

        double timeout = cmd.getOr("to", 0.0);
        // estimator parameters
        u64 Nd = cmd.getOr("Nd", 10);
        u64 Ng = cmd.getOr("Ng", 50);
        u64 iter = cmd.getOr("iter", 10);
        u64 nt = cmd.getOr("nt", cmd.isSet("nt") ? std::thread::hardware_concurrency() : 1);
        u64 w = cmd.getOr("w", 1);
        bool rand = cmd.isSet("rand");
        u64 n = cmd.getOr("rand", 100);
        ListDecoder listDecoder = (ListDecoder)cmd.getOr("ld", 1);

        auto algo = (BPAlgo)cmd.getOr("bp", 2);

        // algo994 parameters
        auto trueDist = cmd.isSet("true");
        alg994 = cmd.getOr("algo994", ALG_SAVED);
        num_saved_generators = cmd.getOr("numGen", 5);
        num_cores = nt;
        num_permutations = cmd.getOr("numPerm", 10);
        print_matrices = 0;

        SparseMtx H;
        LdpcDecoder D;
        std::stringstream label;


        label << return_current_time_and_date() << "\n";

        if (e != 2)
            label << "-ldpc -e " << e << " ";


        label << "-r ";
        for (auto rows : rowVec)
            label << rows << " ";

        if (trueDist)
            label << "-true ";
        else
        {
            label << " -ld " << (int)listDecoder << " -bp " << int(algo) << "-Nd " << Nd << " -Ng " << Ng << " -w " << w;
            if (timeout)
                label << " -to " << timeout;
        }
        label << " -nt " << nt << " -trials " << trial;
        if (tStart)
            label << " -tStart " << tStart;

        label << " -seed " << seed;

        if (uniform)
        {
            label << " -u ";
            if (cmd.isSet("cw"))
                label << " -cw " << colWeight;
        }
        else
        {
            if (cmd.isSet("lb"))
                label << "-ld";

            label << " -cw " << colWeight << " -g " << gap << " -dw " << dWeight
                << " -diag " << diag << " -dDiag " << dDiag;

            if (doubleBand.size())
            {
                label << " -db ";
                for (auto db : doubleBand)
                    label << db << " ";
            }

            if (trim)
                label << " -trim ";

            if (extend)
                label << " -extend ";

        }

        std::ofstream log;
        if (logPath.size())
        {
            log.open(logPath, std::ios::out | std::ios::app);
        }

        if (log.is_open())
            log << "\n" << label.str() << std::endl;

        for (auto rows : rowVec)
        {
            u64 cols = rows * e;

            std::vector<u64> dd;

            heatMap.clear();
            heatMap.resize(cols);
            heatMapCount.clear();
            heatMapCount.resize(cols);
            heatSet.clear();

            if (log.is_open())
                log << rows << ": ";

            for (u64 i = tStart; i < trial; ++i)
            {
                oc::PRNG prng(block(seed, i));


                if (uniform)
                {
                    if (cmd.isSet("cw"))
                    {
                        H = sampleFixedColWeight(rows, cols, colWeight, prng, true);
                    }
                    else
                        H = sampleUniformSystematic(rows, cols, prng);
                }
                else if (cmd.isSet("lb"))
                    H = sampleTriangularLongBand(
                        rows, cols,
                        colWeight, gap,
                        dWeight, diag, doubleBand.size(), prng);
                else
                    H = sampleTriangularBand(
                        rows, cols,
                        colWeight, gap,
                        dWeight, diag, dDiag, doubleBand, trim, extend, prng);

                //std::cout << H << std::endl;

                //impulseDist(5, 5000);
                //oc::Timer timer;
                //timer.setTimePoint("");

                //timer.setTimePoint("e");

                if (trueDist)
                {
                    auto d = minDist2(H.dense(), nt, false);
                    dd.push_back(d);
                }
                else
                {
                    auto d = impulseDist(H, Nd, Ng, w, iter, nt, rand, n, algo, listDecoder, verbose, timeout);
                    dd.push_back(d);
                }

                if (log.is_open())
                    log << " " << dd.back();

                if (verbose)
                {
                    std::cout << dd.back();

                    std::cout << "\n" << H << std::endl;
                    for (auto c : minCW)
                    {
                        if (c)
                            std::cout << oc::Color::Green << int(c) << " " << oc::Color::Default;
                        else
                            std::cout << int(c) << " ";
                    }
                    std::cout << std::endl;

                    if (hm || verbose)
                    {
                        u64 max = 0ull;
                        for (u64 i = 0; i < heatMap.size(); ++i)
                        {
                            max = std::max<u64>(max, heatMap[i]);
                        }

                        double tick = max / 10.0;


                        for (u64 j = 1; j <= 10; ++j)
                        {
                            for (u64 i = 0; i < heatMap.size(); ++i)
                            {
                                if (heatMap[i] >= j * tick)
                                    std::cout << "* ";
                                else
                                    std::cout << "  ";
                            }
                            std::cout << "|\n";
                        }
                        std::cout << std::flush;

                        heatMap.clear();
                        heatMap.resize(cols);
                        heatMapCount.clear();
                        heatMapCount.resize(cols);
                        heatSet.clear();
                    }

                }
                else if (!cmd.isSet("silent"))
                {
                    std::cout << dd.back() << " " << std::flush;
                }
                //std::cout << timer << std::endl;;

            }

            if (log.is_open())
                log << std::endl;

            auto tt = trial - tStart;
            auto min = *std::min_element(dd.begin(), dd.end());
            auto max = *std::max_element(dd.begin(), dd.end());
            auto avg = std::accumulate(dd.begin(), dd.end(), 0ull) / double(tt);
            //avg = avg / tt;

            //std::cout << "\r";
            //auto str = ss.str();
            //for (u64 i = 0; i < str.size(); ++i)
            //    std::cout << " ";
            //std::cout << ;

            {
                std::cout << oc::Color::Green << "\r" << rows << ": ";
                std::cout << min << " " << avg << " " << max << " ~ " << oc::Color::Default;
                for (auto d : dd)
                    std::cout << d << " ";

                std::cout << std::endl;
            }



            if (hm && !verbose)
            {
                u64 max = 0ull;
                for (u64 i = 0; i < heatMap.size(); ++i)
                {
                    max = std::max(max, heatMap[i]);
                }

                double tick = max / 10.0;


                for (u64 j = 1; j <= 10; ++j)
                {
                    for (u64 i = 0; i < heatMap.size(); ++i)
                    {
                        if (heatMap[i] >= j * tick)
                            std::cout << "* ";
                        else
                            std::cout << "  ";
                    }
                    std::cout << "|\n";
                }
                std::cout << std::flush;

                heatMap.clear();
                heatMap.resize(cols);
                heatMapCount.clear();
                heatMapCount.resize(cols);
                heatSet.clear();
            }

        }




        return;

    }



}