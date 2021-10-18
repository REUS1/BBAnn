#pragma once

#include "ivf/kmeans.h"
#include "ivf/balanced_kmeans.h"
#include "ivf/same_size_kmeans.h"
#include <set>

template<typename T>
void find_nearest_large_bucket (
        const T *  x, const float * centroids,
        int64_t nx, int64_t k, int64_t dim, int64_t * hassign,
        int64_t * transform_table, std::vector<int64_t>& assign)
{
    std::vector<int64_t> new_assign(nx, -1);
#pragma omp parallel for
    for(int i = 0; i < nx; i++) {
        auto *x_i =  x + i * dim;
        int min_id = 0;
        float min_dist ;
        float dist ;
        if (transform_table[assign[i]] != -1) {
            new_assign[i] = transform_table[assign[i]];
        } else {
            min_dist = L2sqr<const T, const float ,float >(x_i, centroids + transform_table[min_id] * dim, dim);

            for (int j = min_id; j < k; j++) {

                dist = L2sqr<const T, const float, float>(x_i, centroids + transform_table[j] *dim , dim);
                if(dist < min_dist) {
                    min_dist = dist;
                    min_id = j;
                }
            }
            new_assign[i] = min_id;
        }
    }
    assign.assign(new_assign.begin(), new_assign.end());
}

template <typename T>
void merge_clusters(LevelType level, int64_t dim, int64_t nx, int64_t& k, const T* x,
                    std::vector<int64_t>& assign, std::vector<float>& centroids, float avg_len = 0.0) {

    int64_t* hassign = new int64_t [k];
    memset(hassign, 0, sizeof(int64_t) * k);
    for (int i = 0; i < nx; i++) {
        hassign[assign[i]]++;
    }

    int64_t large_bucket_min_limit;
    int64_t small_bucket_max_limit;
    // strategies should be changed according to different scenarios
    if(level == LevelType::FIRST_LEVEL) {

        large_bucket_min_limit = MAX_SAME_SIZE_THRESHOLD;
        small_bucket_max_limit = MAX_SAME_SIZE_THRESHOLD;

    } else {

        large_bucket_min_limit = MAX_SAME_SIZE_THRESHOLD;
        small_bucket_max_limit = MIN_SAME_SIZE_THRESHOLD;

    }

    //find the new k2 and centroids:
    int64_t new_k = 0;
    int64_t large_bucket_num = 0;
    int64_t middle_bucket_num = 0;
    int64_t * transform_table = new int64_t [k]; // old k to new k
    for (int i=0; i < k; i++ ) {
        if(hassign[i] >= large_bucket_min_limit) {
            transform_table[i] = large_bucket_num;
            large_bucket_num++;
        } else {
            transform_table[i] = -1;
        }
    }
    new_k += large_bucket_num;
    for (int i = 0; i < k; i++) {
        if (hassign[i] >= small_bucket_max_limit && transform_table[i] == -1) {
            transform_table[i] = new_k;
            new_k++;
            middle_bucket_num ++;
        }
    }
    if(new_k == k) {

        return ;
    }
    new_k = new_k != 0 ? new_k : 1; // add a bucket for all small bucket

    int64_t * new_hassign = new int64_t [new_k];
    float * new_centroids = new float[dim * new_k];
    for (int i = 0; i < k; i++) {
        if(transform_table[i] != -1) {
            memcpy(new_centroids + transform_table[i] * dim, centroids.data() + i * dim, dim * sizeof(float));
        }
    }
    if (large_bucket_num) {

        find_nearest_large_bucket<T>(x, new_centroids, nx, large_bucket_num, dim, hassign, transform_table, assign);

        compute_centroids<T>(dim, new_k, nx, x, assign.data(), new_hassign, new_centroids, avg_len);

    } else if (middle_bucket_num) {
        find_nearest_large_bucket<T>(x, new_centroids, nx, middle_bucket_num, dim, hassign, transform_table, assign);

        compute_centroids<T>(dim, new_k, nx, x, assign.data(), new_hassign, new_centroids, avg_len);
    } else {

        float * __restrict merge_centroid = new_centroids;
        int64_t merge_centroid_id = 0;
        memset(merge_centroid, 0, sizeof(float) * dim);
#pragma omp parallel for
        for (int i = 0; i < nx; i++) {
            auto * __restrict x_in = x + i * dim;
            if(transform_table[assign[i]] == -1) {
                for (int d = 0; d < dim; d++) {
                    merge_centroid[d] += x_in[d];
                }
                assign[i] = merge_centroid_id;
            } else {
                assign[i] = transform_table[assign[i]];
            }
        }

        if (avg_len != 0.0) {
            float len = avg_len / sqrt(IP<float, float, double>(merge_centroid, merge_centroid, dim));
            for (int64_t j = 0; j < dim; j++){
                merge_centroid[j] *= len;
            }
        } else {
            float norm = 1.0 / nx;
            for (int64_t j = 0; j < dim; j++) {
                merge_centroid[j] *= norm;
            }
        }
    }

    //update meta :
    k = new_k;
    centroids.assign(new_centroids, new_centroids + k * dim);

    delete [] new_centroids;
    delete [] new_hassign;
    delete [] transform_table;
    delete [] hassign;


    return ;
}


template <typename T>
void recursive_kmeans(uint32_t k1_id, int64_t cluster_size, T* data, uint32_t* ids, int64_t dim, uint32_t threshold, const uint64_t blk_size,
                      uint32_t& blk_num, IOWriter& data_writer, IOWriter& centroids_writer, IOWriter& centroids_id_writer, int level,
                      bool kmpp = false, float avg_len = 0.0, int64_t niter = 10, int64_t seed = 1234) {
    //std::cout<< "level" <<level<<" cluster_size"<<cluster_size<<std::endl;

    float weight = 0;
    int vector_size = sizeof(T) * dim;
    int id_size = sizeof(uint32_t);
    int64_t k2;
    bool do_same_size_kmeans = (LevelType (level) >= LevelType ::BALANCE_LEVEL) ||
            (LevelType (level) == LevelType ::THIRTH_LEVEL && cluster_size >= MIN_SAME_SIZE_THRESHOLD && cluster_size <= MAX_SAME_SIZE_THRESHOLD);
    if (do_same_size_kmeans) {
        k2 = std::max((cluster_size + threshold - 1) / threshold, 1L);
    } else {
        k2 = int64_t(sqrt(cluster_size/threshold)) + 1;
        k2 = k2 < MAX_CLUSTER_K2 ? k2 : MAX_CLUSTER_K2;
    }

    //float* k2_centroids = new float[k2 * dim];
    std::vector<float> k2_centroids(k2 * dim, 0.0);
    std::vector<int64_t> cluster_id(cluster_size, -1);

    if(do_same_size_kmeans) {
        //use same size kmeans or graph partition
        k2 = std::max((cluster_size + threshold - 1) / threshold, 1L);
        same_size_kmeans<T>(cluster_size, data, dim, k2, k2_centroids.data(), cluster_id.data(), kmpp, avg_len, niter, seed);

        //  std::cout << "after same size kmeans： "<< cluster_size<<std::endl;
        std::vector<std::pair<float, std::pair<uint32_t, uint32_t>>> vec_to_centroids_dist(cluster_size * k2,
                                                                                           std::pair<float, std::pair<uint32_t, uint32_t>>(0, std::pair<uint32_t, uint32_t>(0,0)));
        //   std::cout<<"calculate the dis"<<std::endl;
#pragma omp parallel for schedule(static)
        for (int i = 0; i < cluster_size; i++) {
            for (int j = 0; j< k2; j++) {
                vec_to_centroids_dist[i * k2 + j].first = L2sqr<const T, const float ,float >(data + i * dim, k2_centroids.data() + j * dim, dim);
                vec_to_centroids_dist[i * k2 + j].second = std::pair<uint32_t, uint32_t>(i, j); //(vec_offest, cent_offest)
            }
        }
        //  std::cout<<"sort"<<std::endl;
        std::sort(vec_to_centroids_dist.begin(), vec_to_centroids_dist.end(),
                  [&](const std::pair<float, std::pair<uint32_t, uint32_t>> &x, const std::pair<float, std::pair<uint32_t, uint32_t>> &y) {
                      return x.first < y.first;
                  });
        // std::cout <<"the first one "<<vec_to_centroids_dist[0].first<<"the last one "<<vec_to_centroids_dist[vec_to_centroids_dist.size()-1].first<<std::endl;
        // std::cout<<"coay data"<<std::endl;
        //malloc the flush buffer:
        char * buf = new char [blk_size * k2];
        memset(buf, 0 , blk_size * k2);
        uint64_t entry_size = dim * sizeof(T) + sizeof(uint32_t);
        uint64_t entry_num = blk_size / entry_size;
        assert(entry_num > 0);
        std::vector<std::set<uint32_t>> blk_mask(k2, std::set<uint32_t>());
        for (int i =0 ; i < cluster_size; i++) {
            blk_mask[cluster_id[i]].insert(i);
        }
        uint64_t total_insert_size = cluster_size;
        uint64_t max_total_insert_size = k2 * entry_num;
        for (int i = 0; total_insert_size < max_total_insert_size && i < vec_to_centroids_dist.size(); i++) {
            auto cen_id = vec_to_centroids_dist[i].second.second;
            auto vec_id = vec_to_centroids_dist[i].second.first;
            if(blk_mask[cen_id].size() < entry_num && blk_mask[cen_id].count(vec_id) != -1) {
                blk_mask[cen_id].insert(vec_id);
                total_insert_size ++;
            }
        }
        std::cout<<"write back"<<std::endl;
        //write back to disk buf
#pragma omp parallel for schedule(static)
        for (int i = 0; i < k2; i++) {
            char * cen_buf = buf + blk_size * i;
            *reinterpret_cast<uint32_t*>(cen_buf) = blk_mask[i].size();
            std::cout <<"block size "<< blk_mask[i].size()<<std::endl;
            char * insert_pos = cen_buf + sizeof(uint32_t);
            for (auto iter = blk_mask[i].begin(); iter != blk_mask[i].end(); iter++) {
                //   std::cout<<"i write back"<<i<<" "<<*iter<<std::endl;
                memcpy(insert_pos, data + (int64_t)(*iter) * dim, dim * sizeof(T));
                memcpy(insert_pos + dim * sizeof(T), &(ids[*iter]), sizeof(uint32_t));
                insert_pos += entry_size;
            }
            assert (insert_pos < cen_buf + blk_size);
        }
        std::cout<<"after write back"<<std::endl;
        data_writer.write((char*)buf, k2 * blk_size);
        std::cout<<"write cen"<<std::endl;
        //write back centroids
        for (int i = 0; i < k2; i++) {
            uint32_t glb_id = gen_global_block_id(k1_id, blk_num);
            centroids_writer.write((char *)(k2_centroids.data() + i * dim), sizeof(float) * dim);
            centroids_id_writer.write((char * )(&glb_id), sizeof(uint32_t));
            blk_num++;
        }
        delete [] buf;
        buf= nullptr;
        std::cout<<"end of write back"<<std::endl;
        return ;

    } else {
        int64_t train_size = cluster_size;
        T* train_data = nullptr;
        if (cluster_size > k2 * K2_MAX_POINTS_PER_CENTROID) {
            train_size = k2 * K2_MAX_POINTS_PER_CENTROID;
            train_data = new T [train_size * dim];
            random_sampling_k2(data, cluster_size, dim, train_size, train_data, seed);
        } else {
            train_data = data;
        }
        kmeans<T>(train_size, train_data, dim, k2, k2_centroids.data(), kmpp, avg_len, niter, seed);
        if(cluster_size > k2 * K2_MAX_POINTS_PER_CENTROID) {
            delete [] train_data;
        }

        // Dynamic balance constraint K-means:
        // balanced_kmeans<T>(cluster_size, data, dim, k2, k2_centroids, weight, kmpp, avg_len, niter, seed);
        std::vector<float> dists(cluster_size, -1);
        if( weight!=0 && cluster_size <= KMEANS_THRESHOLD ) {
            dynamic_assign<T, float, float>(data, k2_centroids.data(), dim, cluster_size, k2, weight, cluster_id.data(), dists.data());
        } else {
            elkan_L2_assign<T, float, float>(data, k2_centroids.data(), dim, cluster_size, k2, cluster_id.data(), dists.data());
        }

        //dists is useless, so delete first
        std::vector<float>().swap(dists);

        merge_clusters<T>((LevelType)level, dim, cluster_size, k2, data, cluster_id, k2_centroids, avg_len);

        //split_clusters_half(dim, k2, cluster_size, data, nullptr, cluster_id.data(), k2_centroids, avg_len);
    }

    std::vector<int64_t> bucket_pre_size(k2 + 1, 0);

    for (int i=0; i<cluster_size; i++) {
        bucket_pre_size[cluster_id[i]+1]++;
    }
    for (int i=1; i <= k2; i++) {
        bucket_pre_size[i] += bucket_pre_size[i-1];
    }

    //reorder thr data and ids by their cluster id
    T* x_temp = new T[cluster_size * dim];
    uint32_t* ids_temp = new uint32_t[cluster_size];
    int64_t offest;
    memcpy(x_temp, data, cluster_size * vector_size);
    memcpy(ids_temp, ids, cluster_size * id_size);
    for(int i=0; i < cluster_size; i++) {
        offest = (bucket_pre_size[cluster_id[i]]++);
        ids[offest] = ids_temp[i];
        memcpy(data + offest * dim, x_temp + i * dim, vector_size);
    }
    delete [] x_temp;
    delete [] ids_temp;

    int64_t bucket_size;
    int64_t bucket_offest;
    int entry_size = vector_size + id_size;
    uint32_t global_id;

    char* data_blk_buf = new char[blk_size];
    for(int i=0; i < k2; i++) {
        if (i == 0) {
            bucket_size = bucket_pre_size[i];
            bucket_offest = 0;
        } else {
            bucket_size = bucket_pre_size[i] - bucket_pre_size[i - 1];
            bucket_offest = bucket_pre_size[i - 1];
        }
        // std::cout<<"after kmeans : centroids i"<<i<<" has vectors "<<(int)bucket_size<<std::endl;
        if (bucket_size <= threshold) {
            //write a blk to file
            //std::cout << bucket_size<<std::endl;
            memset(data_blk_buf, 0, blk_size);
            *reinterpret_cast<uint32_t*>(data_blk_buf) = bucket_size;
            char* beg_address = data_blk_buf + sizeof(uint32_t);

            for (int j = 0; j < bucket_size; j++) {
                memcpy(beg_address + j * entry_size, data + dim * (bucket_offest + j), vector_size);
                memcpy(beg_address + j * entry_size + vector_size, ids + bucket_offest + j, id_size);
            }
            global_id = gen_global_block_id(k1_id, blk_num);

            data_writer.write((char *) data_blk_buf, blk_size);
            centroids_writer.write((char *) (k2_centroids.data() + i * dim), sizeof(float) * dim);
            centroids_id_writer.write((char *) (&global_id), sizeof(uint32_t));
            blk_num++;

        } else {
            recursive_kmeans(k1_id, bucket_size, data + bucket_offest * dim, ids + bucket_offest, dim, threshold, blk_size,
                             blk_num, data_writer, centroids_writer, centroids_id_writer, level + 1, kmpp, avg_len, niter, seed);
        }
    }
    delete [] data_blk_buf;
}