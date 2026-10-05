#pragma once
#include "vodmodels.h"

namespace OKILTV::Vod {
// Durable user choices, independent of provider catalogues and watch progress.
class IVodMovieListsRepository {
public:
    virtual ~IVodMovieListsRepository() = default;
    virtual Result<MovieListState> readMovieLists(const ContentRef &, const RequestContext &) = 0;
    virtual Result<MovieListState> setMovieList(const ContentRef &, MovieList, bool, const RequestContext &) = 0;
};
}
