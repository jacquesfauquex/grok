/*
 *    Copyright (C) 2016-2022 Grok Image Compression Inc.
 *
 *    This source code is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This source code is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "grk_includes.h"

namespace grk
{
const uint8_t gain_b[4] = {0, 1, 1, 2};

DecompressScheduler::DecompressScheduler(TileComponent* tilec,
		 	 	 	 	 	 	 	 	 TileCodingParams* tcp,
										TileComponentCodingParams* tccp,
										uint8_t prec) : Scheduler(tilec->highestResolutionDecompressed+1),
												tilec_(tilec),
												tcp_(tcp),
												tccp_(tccp),
												prec_(prec)
{}

bool DecompressScheduler::schedule(void)
{
	bool wholeTileDecoding = tilec_->isWholeTileDecoding();
	ResDecompressBlocks resBlocks;
	for(uint8_t resno = 0; resno <= tilec_->highestResolutionDecompressed; ++resno)
	{
		auto res = tilec_->tileCompResolution + resno;
		for(uint8_t bandIndex = 0; bandIndex < res->numTileBandWindows; ++bandIndex)
		{
			auto band = res->tileBand + bandIndex;
			auto paddedBandWindow =
				tilec_->getBuffer()->getBandWindowPadded(resno, band->orientation);
			for(auto precinct : band->precincts)
			{
				if(!wholeTileDecoding && !paddedBandWindow->nonEmptyIntersection(precinct))
					continue;
				for(uint64_t cblkno = 0; cblkno < precinct->getNumCblks(); ++cblkno)
				{
					auto cblkBounds = precinct->getCodeBlockBounds(cblkno);
					if(wholeTileDecoding || paddedBandWindow->nonEmptyIntersection(&cblkBounds))
					{
						auto cblk = precinct->getDecompressedBlockPtr(cblkno);
						auto block = new DecompressBlockExec();
						block->x = cblk->x0;
						block->y = cblk->y0;
						block->tilec = tilec_;
						block->bandIndex = bandIndex;
						block->bandNumbps = band->numbps;
						block->bandOrientation = band->orientation;
						block->cblk = cblk;
						block->cblk_sty = tccp_->cblk_sty;
						block->qmfbid = tccp_->qmfbid;
						block->resno = resno;
						block->roishift = tccp_->roishift;
						block->stepsize = band->stepsize;
						block->k_msbs = (uint8_t)(band->numbps - cblk->numbps);
						block->R_b = prec_ + gain_b[band->orientation];
						resBlocks.push_back(block);
					}
				}
			}
		}
		if(!resBlocks.empty() && resno > 0) {
			blocks.push_back(resBlocks);
			resBlocks.clear();
		}
	}
	if(!resBlocks.empty()) {
		blocks.push_back(resBlocks);
		resBlocks.clear();
	}
	// nominal code block dimensions
	uint16_t codeblock_width = (uint16_t)(tccp_->cblkw ? (uint32_t)1 << tccp_->cblkw : 0);
	uint16_t codeblock_height = (uint16_t)(tccp_->cblkh ? (uint32_t)1 << tccp_->cblkh : 0);
	for(auto i = 0U; i < ExecSingleton::get()->num_workers(); ++i)
		t1Implementations.push_back(
			T1Factory::makeT1(false, tcp_, codeblock_width, codeblock_height));

	if(!blocks.size())
		return true;
	size_t num_threads = ExecSingleton::get()->num_workers();
	success = true;
	if(num_threads == 1)
	{
		for(auto& resBlocks : blocks)
		{
			for(auto& block : resBlocks)
			{
				if(!success)
				{
					delete block;
				}
				else
				{
					auto impl = t1Implementations[(size_t)0];
					if(!decompressBlock(impl, block))
						success = false;
				}
			}
		}

		return success;
	}

	uint8_t resno = 0;
	for(auto& resBlocks : blocks)
	{
		assert(resBlocks.size());
		auto resTasks = new tf::Task[resBlocks.size()];
		for(size_t blockno = 0; blockno < resBlocks.size(); ++blockno)
			resTasks[blockno] = state_->resBlockFlows_[resno].placeholder();
		state_->blockTasks_[resno] = resTasks;
		auto name = state_->genResBlockTaskName(resno);
		state_->resBlockTasks_[resno] = state_->codecFlow_.composed_of(state_->resBlockFlows_[resno]).name(name);
		resno++;
	}
	resno = 0;
	for(auto& resBlocks : blocks)
	{
		size_t blockno = 0;
		for(auto& block : resBlocks)
		{
			state_->blockTasks_[resno][blockno++].work([this, block] {
				if(!success)
				{
					delete block;
				}
				else
				{
					auto threadnum = ExecSingleton::get()->this_worker_id();
					auto impl = t1Implementations[(size_t)threadnum];
					if(!decompressBlock(impl, block))
						success = false;
				}
			});
		}
		resno++;
	}

	return true;
}
bool DecompressScheduler::decompressBlock(T1Interface* impl, DecompressBlockExec* block)
{
	try
	{
		bool rc = block->open(impl);
		delete block;
		return rc;
	}
	catch(std::runtime_error& rerr)
	{
		delete block;
		GRK_ERROR(rerr.what());
		return false;
	}

	return true;
}

} // namespace grk
