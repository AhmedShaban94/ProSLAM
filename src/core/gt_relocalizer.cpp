#include "gt_relocalizer.h"

#include "srrg_types/types.hpp"
//#include "DUtils/DUtils.h"
//#include "DUtilsCV/DUtilsCV.h"
//#include "DVision/DVision.h"

namespace gslam {
  using namespace srrg_core;

  Relocalizer::Relocalizer(): _query(0)/*,
                              _bow_database(0)*/ {
    clear();

    //ds initialize jit head
    _query_history_JIT = new QueryFrame*[_number_of_queries_JIT];
    for (Count u = 0; u < 10; ++u) {
      _query_history_JIT[u] = 0;
    }

    _query_history.clear();
    assert(_query_history_queue.empty());
    _aligner = static_cast<XYZAligner*>(AlignerFactory::create(AlignerType6_3::xyz));
    assert(_aligner != 0);
    LOG_INFO("Relocalizer::Relocalizer", "constructed");
  }

  Relocalizer::~Relocalizer() {

    LOG_INFO("Relocalizer::Relocalizer", "destroying");

    //ds free closure buffer
    clear();

    //ds free history
    for (const Query* query: _query_history) {
      for (const HBSTMatchable* matchable: query->matchables) {
        delete matchable;
      }
      delete query;
    }
    _query_history.clear();

    //ds free history buffer
    for (Index i = 0; i < _query_history_queue.size(); ++i) {
      const Query* query = _query_history_queue.front();
      _query_history_queue.pop();
      for (const HBSTMatchable* matchable: query->matchables) {
        delete matchable;
      }
      delete query;
    }

    //ds free active query - if available
    if (_query) {
      delete _query;
    }

    LOG_INFO("Relocalizer::Relocalizer", "destroyed");
  }

//  //ds load bow vocabulary
//  void Relocalizer::loadVocabulary(const std::string& path_to_dbow2_vocabulary_) {
//    LOG_INFO_VALUE("Relocalizer::loadVocabulary", "loading vocabulary", path_to_dbow2_vocabulary_);
//    try {
//      _bow_database = new BriefDatabase(BriefVocabulary(path_to_dbow2_vocabulary_), false);
//    } catch(const std::string& exception_text) {
//      LOG_ERROR("Relocalizer::loadVocabulary", exception_text);
//      throw std::runtime_error(exception_text);
//    }
//  }

  void Relocalizer::init(const KeyFrame* keyframe) {
    CHRONOMETER_START(overall)
    _query = new Query(keyframe);
    CHRONOMETER_STOP(overall)
  }

  //ds integrate frame into loop closing pool
  void Relocalizer::train() {
    CHRONOMETER_START(overall)

    //ds if query is valid
    if (0 != _query && 0 < _query->appearances.size()) {

      //ds add the active query to our database structure
      _query_history_queue.push(_query);

      //ds check if we can pop the first element of the buffer into our history
      if (_query_history_queue.size() > _preliminary_minimum_interspace_queries) {
        _query_history.push_back(_query_history_queue.front());
//        std::cerr << "added to history: " << _query_history_queue.front()->keyframe->index() << std::endl;
        //_query_history.insert(std::make_pair(_bow_database->add(_query_history_queue.front()->descriptors_bow), _query_history_queue.front()));
        _query_history_queue.pop();
      }
    }

    //ds reset handles
    _query = 0;
    CHRONOMETER_STOP(overall)
  }

  void Relocalizer::flush() {
    CHRONOMETER_START(overall)

    //ds for all elements in the current history
    while (_query_history_queue.size() > 0) {
      _query_history.push_back(_query_history_queue.front());
      std::cerr << "flushed to history: " << _query_history_queue.front()->keyframe->index() << std::endl;
      _query_history_queue.pop();
    }

    CHRONOMETER_STOP(overall)
  }

  //ds retrieve loop closure candidates for the given cloud
  void Relocalizer::detect(const bool& force_matching_) {
    CHRONOMETER_START(overall)

    //ds clear output buffers
    clear();

    //ds compute upper closure search id limit
    //const DBoW2::EntryId index_limit = std::max(0, static_cast<int>(_bow_database->size()-_preliminary_minimum_interspace_queries));
    //assert(index_limit >= 0);

    //ds get the query results
    //DBoW2::QueryResults matching_queries;
    //CHRONOMETER_START
    //_bow_database->query(_query->descriptors_bow, matching_queries, _preliminary_maximum_number_of_closures_per_query, index_limit);
    //CHRONOMETER_STOP

    //gg: TO TRY 2
    //and what about "preloading" the tracker with some of the landmarks? Any idea on how to do that?
    
    //ds evaluate queries
    //for (const DBoW2::Result& matching_query: matching_queries ) {
    //  if (matching_query.Score > _preliminary_minimum_score_bow) {

    //ds evaluate all past queries
    for (const Query* reference: _query_history) {

      //ds get match count
      const gt_real matching_ratio = reference->hbst_tree->getMatchingRatioFlat(_query->matchables);

      //ds if acceptable
      if (matching_ratio > _preliminary_minimum_matching_ratio || force_matching_) {

        //ds buffer reference
        //const Query* reference = _query_history.at(matching_query.Id);
        assert(reference != 0);

        //ds matches within the current reference
        HBSTTree::MatchVector matches_unfiltered;
        MatchMap descriptor_matches_pointwise;

        //ds get matches
        assert(0 < _query->matchables.size());
        assert(0 < reference->matchables.size());
        reference->hbst_tree->match(_query->matchables, matches_unfiltered);
        assert(0 < matches_unfiltered.size());
        const Count absolute_number_of_descriptor_matches = matches_unfiltered.size();

        //ds loop over all matches
        for (const HBSTTree::Match match: matches_unfiltered) {

          const Appearance* appearance_query     = _query->appearances[match.identifier_query];
          const Appearance* appearance_reference = reference->appearances[match.identifier_reference];
          const Identifier& query_index          = appearance_query->item->landmark()->index();

          try{

            //ds add a new match to the given query point
            descriptor_matches_pointwise.at(query_index).push_back(new Match(appearance_query->item,
                                                                   appearance_reference->item,
                                                                   match.distance));
          } catch(const std::out_of_range& /*exception*/) {

            //ds initialize the first match for the given query point
            descriptor_matches_pointwise.insert(std::make_pair(query_index, MatchPtrVector(1, new Match(appearance_query->item,
                                                                                              appearance_reference->item,
                                                                                              match.distance))));
          }
        }
        assert(0 < absolute_number_of_descriptor_matches);
        assert(0 < _query->appearances.size());
        assert(0 < descriptor_matches_pointwise.size());
        const gt_real relative_number_of_descriptor_matches_query     = static_cast<gt_real>(absolute_number_of_descriptor_matches)/_query->appearances.size();
        //const gt_real relative_number_of_descriptor_matches_reference = static_cast<gt_real>(absolute_number_of_descriptor_matches)/reference->appearances.size();
        //const gt_real relative_delta = std::fabs(relative_number_of_descriptor_matches_query-relative_number_of_descriptor_matches_reference)/relative_number_of_descriptor_matches_reference;

        //ds if the result quality is sufficient
        if (descriptor_matches_pointwise.size() > _minimum_absolute_number_of_matches_pointwise) {

          //ds correspondences
          CorrespondencePointerVector correspondences;
          _mask_id_references_for_correspondences.clear();

          //ds compute point-to-point correspondences for all matches
          for(const MatchMapElement matches_per_point: descriptor_matches_pointwise){
            const Correspondence* correspondence = getCorrespondenceNN(matches_per_point.second);
            if (correspondence != 0) {
              correspondences.push_back(correspondence);
            }
          }
          assert(0 < correspondences.size());

            //ds update closures
          _closures.push_back(new CorrespondenceCollection(_query->keyframe,
                                          reference->keyframe,
                                          absolute_number_of_descriptor_matches,
                                          relative_number_of_descriptor_matches_query,
                                          descriptor_matches_pointwise,
                                          correspondences));
        } /*else {
          std::cerr << _query->keyframe->index() << " | " << reference->keyframe->index() << " not enough matches: " << descriptor_matches_pointwise.size() << std::endl;
        }*/
      }
    }
    CHRONOMETER_STOP(overall)
  }

  //ds geometric verification
  void Relocalizer::compute() {
    CHRONOMETER_START(overall)
    for(CorrespondenceCollection* closure: _closures) {
      _aligner->init(closure);
      _aligner->converge();
    }
    CHRONOMETER_STOP(overall)
  }

  const Correspondence* Relocalizer::getCorrespondenceNN(const MatchPtrVector& matches_) {
    assert(0 < matches_.size());

    //ds point counts
    std::multiset<Count> counts;

    //ds best match and count so far
    const Match* match_best = 0;
    Count count_best        = 0;

    //ds loop over the list and count entries
    for(const Match* match: matches_){

      //ds update count - if not in the mask
      if(0 == _mask_id_references_for_correspondences.count(match->item_reference->landmark()->index())) {
        counts.insert(match->item_reference->landmark()->index());
        const Count count_current = counts.count(match->item_reference->landmark()->index());

        //ds if we get a better count
        if( count_best < count_current ){
          count_best = count_current;
          match_best = match;
        }
      }
    }

    if(match_best != 0 && count_best > _minimum_matches_per_correspondence ) {

      //ds block matching against this point by adding it to the mask
      _mask_id_references_for_correspondences.insert(match_best->item_reference->landmark()->index());

      //ds return the found correspondence
      return new Correspondence(match_best->item_query,
                                match_best->item_reference,
                                count_best, static_cast<gt_real>(count_best)/matches_.size());
    }

    return 0;
  }

  void Relocalizer::clear() {
    for(const CorrespondenceCollection* closure: _closures) {
      delete closure;
    }
    _closures.clear();
    _mask_id_references_for_correspondences.clear();
  }

  //ds JIT: retrieve loop closure candidates from chain
  void Relocalizer::train(const Frame* frame_) {

    //ds free last query if set
    if (_query_history_JIT[0] != 0) {
      delete _query_history_JIT[0];
    }

    //ds update chain
    for (Count u = 0; u < 9; ++u) {
      _query_history_JIT[u] = _query_history_JIT[u+1];
    }

    //ds get a new query and set it to the holder
    _query_history_JIT[9] = new QueryFrame(frame_);
  }

  //ds JIT: retrieve loop closure candidates from chain
  void Relocalizer::detect(const Frame* frame_) {

    //ds buffer query
    const QueryFrame* query = _query_history_JIT[9];

    //ds directly look for candidates in the chain (NO PRELIMINARY PHASE)
    for (Count u = 0; u < 9; ++u) {

      //ds start from most recent JIT frame
      const QueryFrame* reference = _query_history_JIT[8-u];

      //ds if available and filled and cross tracking context
      if (reference != 0                                                        &&
          reference->appearances.size() > 0                                     &&
          reference->frame->trackingContext() != query->frame->trackingContext()) {

        //ds matches within the current reference
        HBSTTree::MatchVector matches_unfiltered;
        MatchMap descriptor_matches_pointwise;

        //ds get matches
        assert(0 < query->matchables.size());
        assert(0 < reference->matchables.size());
        reference->hbst_tree->match(query->matchables, matches_unfiltered);
        assert(0 < matches_unfiltered.size());
        //const Count absolute_number_of_descriptor_matches = matches_unfiltered.size();

        //ds loop over all matches
        for (const HBSTTree::Match match: matches_unfiltered) {

          const Appearance* appearance_query     = query->appearances[match.identifier_query];
          const Appearance* appearance_reference = reference->appearances[match.identifier_reference];
          const Identifier& query_index          = appearance_query->item->landmark()->index();

          try{

            //ds add a new match to the given query point
            descriptor_matches_pointwise.at(query_index).push_back(new Match(appearance_query->item,
                                                                   appearance_reference->item,
                                                                   match.distance));
          } catch(const std::out_of_range& /*exception*/) {

            //ds initialize the first match for the given query point
            descriptor_matches_pointwise.insert(std::make_pair(query_index, MatchPtrVector(1, new Match(appearance_query->item,
                                                                                              appearance_reference->item,
                                                                                              match.distance))));
          }
        }
        //assert(0 < absolute_number_of_descriptor_matches);
        assert(0 < query->appearances.size());
        assert(0 < descriptor_matches_pointwise.size());
        //const gt_real relative_number_of_descriptor_matches_query     = static_cast<gt_real>(absolute_number_of_descriptor_matches)/_query->appearances.size();
        //const gt_real relative_number_of_descriptor_matches_reference = static_cast<gt_real>(absolute_number_of_descriptor_matches)/reference->appearances.size();
        //const gt_real relative_delta = std::fabs(relative_number_of_descriptor_matches_query-relative_number_of_descriptor_matches_reference)/relative_number_of_descriptor_matches_reference;

        std::cerr << descriptor_matches_pointwise.size() << std::endl;

        //ds if the result quality is sufficient
        if (descriptor_matches_pointwise.size() > _minimum_absolute_number_of_matches_pointwise) {

        }
      }
    }
  }

} //namespace gtracker