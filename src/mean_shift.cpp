/**
   This file is part of Inpaint.

   Copyright Christoph Heindl 2014

   Inpaint is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   
   Inpaint is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with Inpaint.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <inpaint/mean_shift.h>
#include <opencv2/opencv.hpp>
#include <unordered_set>
#include <vector>

namespace Inpaint {

    /** Convert from feature to bin */
    void toBin(const cv::Mat &feature, cv::Mat_<int> &bin, float bandwidth)
    {
        feature.convertTo(bin, CV_32SC1, 1.f / bandwidth);
    }

    /** Convert bin to feature */
    void toFeature(const cv::Mat_<int> &bin, cv::Mat &feature, float bandwidth)
    {
        bin.convertTo(feature, CV_32FC1, bandwidth);
    }

    struct BinHasher
    {
        std::size_t operator()(const cv::Mat_<int> & k) const {
            std::size_t h = 0;
            const int *r = k.ptr<int>(0);
        
            for (int i = 0; i < k.cols; ++i) {
                combine(h, r[i]);
            }

            return h;
        }

        void combine(std::size_t &seed, int val) const {
            seed ^= ihash(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }

        std::hash<int> ihash;
    };

    struct BinEqual 
    {
        bool operator() (const cv::Mat_<int> &x, const cv::Mat_<int> &y) const 
        { 
            return cv::norm(x, y, cv::NORM_L1) == 0; 
        }
    };

    typedef std::unordered_set<
        cv::Mat_<int>,
        BinHasher,
        BinEqual> BinSet;



    cv::Mat findSeeds(cv::Mat features, float bandwidth)
    {
        BinSet bs;

        cv::Mat_<int> bin;
        for (int y = 0; y < features.rows; ++y) {
            toBin(features.row(y), bin, bandwidth);
            bs.insert(bin);
        }


        cv::Mat_<float> seeds((int)bs.size(), features.cols);
        int count = 0;
        for (auto iter = bs.begin(); iter != bs.end(); ++iter, ++count) {
            toFeature(*iter, seeds.row(count), bandwidth);
        }

        return seeds;
    }

    /** Perform the actual mean-shift. Results will be directly placed in seeds. Additionally outputs the number of inliers for each cluster. */
    void performMeanShift(cv::Mat features, cv::Mat seeds, cv::Mat supports, cv::Mat weights, float bandwidth, int maxIterations, bool perturbate)
    {
        // Setup search structure and take care about the curse of dimensionality.
        cv::flann::KDTreeIndexParams kdtree(1);
        cv::flann::LinearIndexParams linear;
        const bool canUseKdTree = features.cols < 10 && features.rows > 20;
        cv::flann::IndexParams &params = canUseKdTree ? (cv::flann::IndexParams &)kdtree : (cv::flann::IndexParams &)linear;       
        cv::flann::Index index(features, params);
         
        const double stopThreshold = bandwidth / 1000.0;
        cv::Mat_<float> lowerPerturbationRange(1, seeds.cols), upperPerturbationRange(1, seeds.cols); 
        lowerPerturbationRange.setTo(-bandwidth / 2);
        upperPerturbationRange.setTo(bandwidth / 2);

        // Could easily parallelize this outer loop 
        for(int seedId = 0; seedId < seeds.rows; ++seedId) {

            cv::Mat_<int> nbrs(1, features.rows);
            cv::Mat_<float> dists(1, features.rows);

            cv::Mat seed = seeds.row(seedId);
            cv::Mat oldSeed;
            cv::Mat seedWhenPerturbationStarted;
            cv::Mat_<float> perturbationVector(1, seeds.cols);   
            cv::RNG rng(cv::getTickCount()); // Todo take care of this initialization when parallelizing.
            
            bool wasPerturbated = false;            
            int iter = 0;
            int support = 0;

            while (iter < maxIterations)
            {
                support = index.radiusSearch(seed, nbrs, dists, bandwidth, nbrs.cols);
                if (support == 0)
                    break;

                seed.copyTo(oldSeed);

                // Compute new mean
                seed.setTo(0);
                float wSum = 0;
                for (int nbr = 0; nbr < support; ++nbr) {
                    int nbrId = nbrs(0, nbr);
                    const float w = weights.at<float>(0, nbrId);
                    seed += (features.row(nbrId) * w);
                    wSum += w;
                }
                seed /= wSum;

                // Test for convergence
                if (cv::norm(seed, oldSeed) < stopThreshold) {
                    
                    // Completed if we either don't need to perturbate, the number of iterations left is too low, 
                    // or new seed is close to position when perturbation started.

                    if (!perturbate || 
                       (maxIterations - iter < 10) || 
                       (wasPerturbated && cv::norm(seed, seedWhenPerturbationStarted) < stopThreshold)) 
                    {
                        // done
                        break;
                    } else if (perturbate) {
                        seed.copyTo(seedWhenPerturbationStarted);
                        rng.fill(perturbationVector, cv::RNG::UNIFORM, lowerPerturbationRange, upperPerturbationRange);
                        seed += perturbationVector;
                        wasPerturbated = true;
                    }
                }

                ++iter;
            }

            supports.at<int>(0, seedId) = support;
        }
    }

    void mergeClusters(cv::Mat clusters, cv::Mat supports, float bandwidth)
    {
        std::vector<bool> keep(clusters.rows, true);

        cv::flann::KDTreeIndexParams kdtree(1);
        cv::flann::LinearIndexParams linear;
        const bool canUseKdTree = clusters.cols < 10 && clusters.rows > 20;
        cv::flann::IndexParams &params = canUseKdTree ? (cv::flann::IndexParams &)kdtree : (cv::flann::IndexParams &)linear;       
        cv::flann::Index index(clusters, params);

        // Basic procedure: For each cluster, find its nearest neighbor. If the nearest neighbor is within the bandwidth,
        // merge (weighted average, weight = support) the two clusters.

        cv::Mat_<int> nbr(1, 1);
        cv::Mat_<float> dists(1, 1);
        cv::Mat average(1, clusters.cols, CV_32FC1);

        for (int c = 0; c < clusters.rows; ++c) {
            if (!keep[c])
                continue;

            cv::Mat cl = clusters.row(c);
            
            index.knnSearch(cl, nbr, dists, 1);
            if (dists.at<float>(0,0) < bandwidth) {
                float sumWeights = 0;
                average.setTo(0);
                
                float w = (float)supports.at<int>(0, c);
                average += (cl * w);
                sumWeights += w;

                int otherId = nbr.at<int>(0,0);
                w = (float)supports.at<int>(0, otherId);
                average += (clusters.row(otherId) * w);
                sumWeights += w;

                average /= sumWeights;
                keep[otherId] = false;
            }
        }

        int remainingClusters = (int)std::count(keep.begin(), keep.end(), true);
        
        cv::Mat finalClusters(remainingClusters, clusters.cols, CV_32FC1);

        int copyTo = 0;
        for (int i = 0; i < clusters.rows; ++i) {
            if (keep[i]) {
                clusters.row(i).copyTo(finalClusters.row(copyTo));
                ++copyTo;
            }
        }

        finalClusters.copyTo(clusters);
    }

    void assignFeaturesToClusters(cv::Mat features, cv::Mat clusters, cv::Mat labels, cv::Mat distances)
    {
        cv::flann::KDTreeIndexParams kdtree(1);
        cv::flann::LinearIndexParams linear;
        const bool canUseKdTree = clusters.cols < 10 && clusters.rows > 20;
        cv::flann::IndexParams &params = canUseKdTree ? (cv::flann::IndexParams &)kdtree : (cv::flann::IndexParams &)linear;       
        cv::flann::Index index(clusters, params);

        cv::Mat_<int> nbr(1, 1);
        cv::Mat_<float> dists(1, 1);

        for (int i = 0; i < features.rows; ++i) {
            index.knnSearch(features.row(i), nbr, dists, 1);
            labels.at<int>(0, i) = nbr.at<int>(0,0);
            distances.at<float>(0, i) = dists.at<float>(0,0);
        }
    }


    void meanShift(
        cv::InputArray features_, cv::InputArray seeds_, cv::InputArray weights_, 
        cv::OutputArray centers_, cv::OutputArray labels_, cv::OutputArray distances_,   
        float bandwidth, int maxIterations, bool perturbate)
    {
        CV_Assert(
            features_.type() == CV_MAKETYPE(1, CV_32F) &&
            maxIterations > 0);

        cv::Mat features = features_.getMat();
        cv::Mat seeds;
        cv::Mat weights;
        
        // Deal with seeds
        if (!seeds_.empty()) {
            CV_Assert(
                seeds_.type() == CV_MAKETYPE(1, CV_32F) &&
                seeds_.cols() == features_.cols());

            seeds = seeds_.getMat().clone();
        } else {
            // Use binning to determine seeds.
            seeds = findSeeds(features, bandwidth);
        }

        // Deal with weights
        if (!weights_.empty()) {
            CV_Assert(
                weights_.type() == CV_MAKETYPE(1, CV_32F) &&
                weights_.cols() == features_.rows());

            weights = weights_.getMat();
        } else {
            weights.create(1, features_.rows(), CV_32FC1);
            weights.setTo(1);
        }

        // Run mean-shift
        cv::Mat supports(1, seeds.rows, CV_32SC1);
        performMeanShift(features, seeds, supports, weights, bandwidth, maxIterations, perturbate);

        // Merge clusters that are too close.
        mergeClusters(seeds, supports, bandwidth);
        if (centers_.needed()) {
            centers_.create(seeds.size(), CV_32FC1);
            seeds.copyTo(centers_.getMat());
        }

        // Assign labels to features if required.
        if (labels_.needed() || distances_.needed()) {
            labels_.create(1, features.rows, CV_32SC1);
            distances_.create(1, features.rows, CV_32FC1);
            assignFeaturesToClusters(features, seeds, labels_.getMat(), distances_.getMat());
        }
    }
}