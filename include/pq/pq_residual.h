#pragma once

#include "util/heap.h"
#include "util/distance.h"

#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <omp.h>

#include <iostream>
#include <memory>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <limits>
#include <functional>
#include "util/utils.h"
#include "ivf/clustering.h"

template<typename T>
using PQ_Computer = std::function<float(const T*, const float*, int n)>;

// T: type of the database vector
// U: type of codes ID (PQ representation)
template<class C, typename T, typename U>
class PQResidualQuantizer {
private:
    int64_t d, dsub, K, ntotal, npos;
    uint32_t m, nbits;
    MetricType metric_type;

    float* centroids = nullptr;
    U* codes = nullptr;
public:
    PQResidualQuantizer(int64_t _d, uint32_t _m, uint32_t _nbits, MetricType _metric_type)
    : d(_d), m(_m), nbits(_nbits), metric_type(_metric_type), ntotal(0), npos(0) {
        assert(sizeof(float) % sizeof(U) == 0);
        assert(d % m == 0);

        dsub = d / m;
        K = 1 << nbits;

        centroids = new float[m * K * dsub];
        codes = nullptr;

        // avx2 assertion
        assert(K % 32 == 0);
        assert(dsub <= 8);
    };

    ~PQResidualQuantizer() {
        if (centroids != nullptr) {
            delete[] centroids;
        }

        if (codes != nullptr) {
            delete[] codes;
        }
    }

    float* get_centroids() {
        return centroids;
    }

    U* get_codes() {
        return codes;
    }

    uint32_t getM() {
        return m;
    }

    // Code Size = Code Num * sizeof(U)
    int64_t getCodeNum() {
        if (metric_type == MetricType::L2) {
            return m + sizeof(float) / sizeof(U);
        } else {
            return m;
        }
    }

    void init_codes(int64_t _ntotal) {
        if (ntotal < _ntotal) {
            ntotal = _ntotal;
            if (codes != nullptr) {
                delete[] codes;
            }
            codes = new U[ntotal * getCodeNum()];
        }
        npos = 0;
    }

    // get one vector centroid
    void reconstruct(float* r, const U* c) {
        for (uint32_t i = 0; i < m; ++i, ++c) {
            memcpy(
                r + i * dsub,
                centroids + (i * K + (*c)) * dsub,
                dsub * sizeof(float));
        }
    }

    void show_centroids() {
        std::cout << "show pq.centroids:" << std::endl;
        auto pc = centroids;
        for (int64_t i = 0; i < m; i ++) {
            std::cout << "m = " << i << std::endl;
            for (int64_t j = 0; j < K; j ++) {
                std::cout << j << ": (";
                for (int64_t k = 0; k < dsub; k ++)
                    std::cout << *pc ++ << " ";
                std::cout << ")" << std::endl;
            }
            std::cout << std::endl;
        }
    }

    void show_pretab(const float* precompute_table) {
        assert(precompute_table != nullptr);
        std::cout << "show pq.precompute_table:" << std::endl;
        auto pp = precompute_table;
        for (int64_t i = 0; i < m; i ++) {
            std::cout << "m = " << std::endl;
            for (int64_t j = 0; j < K; j ++)
                std::cout << "(" << j << ", " << *pp++ << ") ";
            std::cout << std::endl;
        }
    }

    void calc_precompute_table(float*& precompute_table, const T* q) {
        if (precompute_table == nullptr) {
            precompute_table = new float[K * m];
        }

        for (int64_t i = 0; i < m; ++i) {
            compute_lookuptable_IP<const T>(
                q + i * dsub,
                centroids + i * K * dsub,
                precompute_table + i * K,
                dsub,
                K);
        }
    }

    void train(int64_t n, const T* x, const float* sample_ivf_cen);

    void encode_vectors(float*& precomputer_table,
                        int64_t n, const T* x,
                        const float* ivf_cen);

    void encode_vectors_and_save(
            float*& precomputer_table,
            int64_t n,
            const T* x,
            const float* ivf_cen,
            const std::vector<uint32_t>& buckets,
            const std::string& file_path);

    void search(
            float* precompute_table,
            const T* q,
            const float* centroid,
            const U* pcodes,
            int64_t n,
            int64_t topk,
            typename C::T* values,
            typename C::TI* labels,
            bool reorder, 
            bool heapify,
#ifndef TEST_RPQ
            const uint32_t& cid,
#else
            const uint64_t cid,
#endif
            const uint32_t& off,
            const uint32_t& qid);

    void save_centroids(const std::string& save_file) {
        uint32_t num_centroids = m * K;
        uint32_t dim_centroids = dsub;
        std::ofstream centroids_writer(save_file, std::ios::binary);
        centroids_writer.write((char*)&num_centroids, sizeof(uint32_t));
        centroids_writer.write((char*)&dim_centroids, sizeof(uint32_t));
        centroids_writer.write((char*)centroids, sizeof(float) * K * d);
        centroids_writer.close();
    }

    void load_centroids(const std::string& load_file) {
        uint32_t num_centroids ,dim_centroids;
        std::ifstream centroids_reader(load_file, std::ios::binary);
        centroids_reader.read((char*)&num_centroids, sizeof(uint32_t));
        centroids_reader.read((char*)&dim_centroids, sizeof(uint32_t));
        centroids_reader.read((char*)centroids, sizeof(float) * K * d);
        centroids_reader.close();

        float* new_centroids = new float[m * K * dsub];
        assert(new_centroids != nullptr);

        for (uint32_t i = 0; i < m; ++i) {
            const auto t = i * K * dsub;
            matrix_transpose(centroids + t, new_centroids + t, K, dsub);
        }
        delete[] centroids;
        centroids = new_centroids;
    }
};

template<class C, typename T, typename U>
void PQResidualQuantizer<C, T, U>::train(int64_t n, const T* x, const float* sample_ivf_cen) {
    float* rs = new float[n * dsub];

    for (int64_t i = 0; i < m; ++i) {
        auto xd = x + i * dsub;
        auto cd = sample_ivf_cen + i * dsub;
        int64_t code_cnt = 0;
        for (int64_t j = 0; j < n; ++j, xd += d, cd += d) {
            compute_residual<const T, const float, float>(xd, cd, rs + code_cnt * dsub, dsub);

            // check
            int64_t k;
            for (k = 0; k < code_cnt; ++k) {
                if (memcmp(rs + code_cnt * dsub, rs + k * dsub, dsub * sizeof(float)) == 0) {
                    break;
                }
            }
            if (k == code_cnt) {
                code_cnt++;
            }
        }

        kmeans(code_cnt, rs, dsub, K, centroids + i * K * dsub, true);
    }

    delete[] rs;
};

template<class C, typename T, typename U>
void PQResidualQuantizer<C, T, U>::encode_vectors(float*& precomputer_table,
                                               int64_t n, const T *x,
                                               const float* ivf_cen) {
    assert(npos + n <= ntotal);
    assert(ivf_cen != nullptr);

    auto code_num = getCodeNum();
    U* c = codes + npos * code_num;

    bool new_precomputer_table = false;
    if (precomputer_table == nullptr) {
        precomputer_table = new float[(int64_t)m * K * (K - 1) / 2];
        new_precomputer_table = true;
    }

    float* r = new float[dsub * omp_get_max_threads()];
    assert(r != nullptr);

    for (int64_t loop = 0; loop < m; loop++) {
        float *data = precomputer_table + loop * K * (K - 1) / 2;
        float* cen = centroids + loop * K * dsub;

        auto Y = [&](int64_t i, int64_t j) -> float& {
            assert(i != j);
            return (i > j) ? data[j + i * (i - 1) / 2] : data[i + j * (j - 1) / 2];
        };

        if (new_precomputer_table) {
#pragma omp parallel
            {
                int64_t nt = omp_get_num_threads();
                int64_t rank = omp_get_thread_num();
                for (int64_t i = 1 + rank; i < K; i += nt) {
                    float* y_i = cen + i * dsub;
                    for (int64_t j = 0; j < i; j++) {
                        float* y_j = cen + j * dsub;
                        Y(i, j) = L2sqr<float,float,float>(y_i, y_j, dsub);
                    }
                }
            }
        }

#pragma omp parallel
        {
            int64_t rank = omp_get_thread_num();
            float* rd = r + rank * dsub;
#pragma omp for
            for (int64_t i = 0; i < n; i++) {
                const T* x_i = x + i * d + loop * dsub;
                compute_residual<const T, const float, float>(x_i, ivf_cen + loop * dsub, rd, dsub);

                int64_t ids_i = 0;
                float val_i = L2sqr<const float,const float,float>(rd, cen, dsub);
                float val_i_time_4 = val_i * 4;
                for (int64_t j = 1; j < K; j++) {
                    if (val_i_time_4 <= Y(ids_i, j)) {
                        continue;
                    }
                    const float *y_j = cen + j * dsub;
                    float disij = L2sqr<const float,const float,float>(rd, y_j, dsub);
                    if (disij < val_i) {
                        ids_i = j;
                        val_i = disij;
                        val_i_time_4 = val_i * 4;
                    }
                }

                c[i * code_num + loop] = ids_i;
            }
        }
    }

    npos += n;
    delete[] r;
}

template<class C, typename T, typename U>
void PQResidualQuantizer<C, T, U>::encode_vectors_and_save(
        float*& precomputer_table,
        int64_t n,
        const T* x,
        const float* ivf_cen,
        const std::vector<uint32_t>& buckets,
        const std::string& file_path) {
    assert(ivf_cen != nullptr);

    auto code_num = getCodeNum();
    init_codes(n);

    const float* ivf_c = ivf_cen;
    const T* xd = x;
    for (size_t i = 0; i < buckets.size(); ivf_c += d, xd += buckets[i] * d, ++i) {
        encode_vectors(precomputer_table, buckets[i], xd, ivf_c);
    }

    if (MetricType::L2 == metric_type) {
        // these codes are local, for each cluster
        U* c = get_codes();
        float* r = new float[d];
        ivf_c = ivf_cen;

        // precompute term2
        int64_t cnt = 0;
        for (size_t i = 0; i < buckets.size(); ++i, ivf_c += d) {
            for (uint32_t j = 0; j < buckets[i]; ++j, c += code_num) {
                reconstruct(r, c);
                float term2 = IP<const float, const float, float>(r, r, d) +
                              2.0f * IP<const float, const float, float>(ivf_c, r, d);
                *reinterpret_cast<float*>(c + m) = term2;
                ++cnt;
            }
        }
        delete[] r;
        assert(cnt == n);

#ifndef TEST_RPQ
        uint32_t wm = code_num;
        std::ofstream code_writer(file_path, std::ios::binary | std::ios::out);
        code_writer.write((char*)&n, sizeof(uint32_t));
        code_writer.write((char*)&wm, sizeof(uint32_t));
        code_writer.write((char*)get_codes(), n * code_num * sizeof(U));
        code_writer.close();
#endif

        std::cout << "PQResidualQuantizer encode " << n << " vectors with m = " << m << " and term2 into file "
                  << file_path << std::endl;

    } else if (MetricType::IP == metric_type) {
        std::ofstream code_writer(file_path, std::ios::binary | std::ios::out);
        code_writer.write((char*)&n, sizeof(uint32_t));
        code_writer.write((char*)&m, sizeof(uint32_t));
        code_writer.write((char*)get_codes(), n * m * sizeof(U));
        code_writer.close();

        std::cout << "ProductQuantizer encode " << n << " vectors with m = " << m << " into file "
                  << file_path << std::endl;
    } else {
        std::cerr << "Unrecognized metric type: " << static_cast<int>(metric_type) << std::endl;
    }
}

template <class C, typename T, typename U>
void PQResidualQuantizer<C, T, U>::search(
        float* precompute_table,
        const T* q,             // one query
        const float* centroid,  // one centroid
        const U* pcodes,        // bucket code begin
        int64_t n,              // row count in bucket
        int64_t topk,
        typename C::T* values,
        typename C::TI* labels,
        bool reorder, 
        bool heapify,
#ifndef TEST_RPQ
        const uint32_t& cid,
#else
        const uint64_t cid,
#endif
        const uint32_t& off,
        const uint32_t& qid) {

    assert(precompute_table != nullptr);
    const float* dis_tab = precompute_table;

    auto* __restrict val_ = values;
    auto* __restrict ids_ = labels;

    if (heapify) {
        heap_heapify<C>(topk, val_, ids_);
    }

    const U* c = pcodes;

    if (MetricType::L2 == metric_type) {
        // term 1
        float term1 = L2sqr<const T, const float, float>(q, centroid, d);

        for (int64_t j = 0; j < n; ++j) {
            float dis = term1;

            // term 3
            const float* __restrict dt = dis_tab;
            float term3 = 0;
            for (uint32_t mm = 0; mm < m; ++mm, dt += K) {
                term3 += dt[*c++];
            }
            dis -= 2.0f * term3;

            // term 2
            dis += *reinterpret_cast<const float *>(c);

            c += sizeof(float);

            if (C::cmp(val_[0], dis)) {
#ifndef TEST_RPQ
                heap_swap_top<C>(topk, val_, ids_, dis, gen_refine_id(cid, off + j, qid));
#else
                int32_t* id = (int32_t*)cid;
                heap_swap_top<C>(topk, val_, ids_, dis, id[j]);
#endif
            }
        }
    } else {
        // compute qc
        float qc = IP<const T, const float, float>(q, centroid, d);

        for (int64_t j = 0; j < n; ++j) {
            float dis = qc;
            // compute qr
            const float* __restrict dt = dis_tab;
            for (uint32_t mm = 0; mm < m; ++mm, dt += K) {
                dis += dt[*c++];
            }

            if (C::cmp(val_[0], dis)) {
#ifndef TEST_RPQ
                heap_swap_top<C>(topk, val_, ids_, dis, gen_refine_id(cid, off + j, qid));
#else
                int32_t* id = (int32_t*)cid;
                heap_swap_top<C>(topk, val_, ids_, dis, id[j]);
#endif
            }
        }
    }

    if (reorder) {
        heap_reorder<C>(topk, val_, ids_);
    }
}